#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/spi/spi.h>

#include <linux/kthread.h> 
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timer.h>

#include <linux/fb.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/vmalloc.h>

#include <linux/gpio.h>
#include <linux/uaccess.h>

#define LCDWIDTH 400
#define LCDHEIGHT 240
// --- FIX: Define the *correct* videomemory size for 1bpp ---
#define VIDEOMEMSIZE    12288   /* 12000 bytes */

char commandByte = 0b10000000;
char vcomByte    = 0b01000000;
char clearByte   = 0b00100000;
char paddingByte = 0b00000000;

/*
 * MODIFICATION:
 * Changed GPIOs from old BCM numbers to new kernel offset numbers
 * based on your /sys/kernel/debug/gpio output.
 * Changed type from char to int.
 */
int DISP        = 536; // Was BCM 24
int SCS         = 535; // Was BCM 23
int VCOM        = 537; // Was BCM 25

int lcdWidth = LCDWIDTH;
int lcdHeight = 240;
int fpsCounter;

static int seuil = 4; // Indispensable pour fbcon
module_param(seuil, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP );

char vcomState;

unsigned char lineBuffer[LCDWIDTH/8];

struct sharp {
    struct spi_device	*spi;
	int			id;
    char			name[sizeof("sharp-3")];

    struct mutex		mutex;
	struct work_struct	work;
	spinlock_t		lock;
};

struct sharp   *screen;
struct fb_info *info;

// --- FIX: Add a global pointer for our screen cache ---
static unsigned char *screenCache;

static void *videomemory;
static u_long videomemorysize = VIDEOMEMSIZE;

// --- ID TABLE ---
static const struct spi_device_id sharp_id[] = {
    { "sharp", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, sharp_id);

void vfb_fillrect(struct fb_info *p, const struct fb_fillrect *region);
static int vfb_mmap(struct fb_info *info, struct vm_area_struct *vma);
void sendLine(char *buffer, char lineNumber);

static struct fb_var_screeninfo vfb_default = {
    .xres =   LCDWIDTH,
    .yres =   LCDHEIGHT,
    .xres_virtual = LCDWIDTH,
    .yres_virtual = LCDHEIGHT,
    // --- FIX: Change to 1 bit per pixel ---
    .bits_per_pixel = 1,
    .grayscale = 1,
    // --- FIX: Define a 1-bit format ---
    .red =    { 0, 1, 0 },
    .green =  { 0, 1, 0 },
    .blue =   { 0, 1, 0 },
    .activate = FB_ACTIVATE_NOW,
    .height = 400,
    .width =  240,
    .pixclock = 20000,
    .left_margin =  0,
    .right_margin = 0,
    .upper_margin = 0,
    .lower_margin = 0,
    .hsync_len =    128,
    .vsync_len =    128,
    .vmode =  FB_VMODE_NONINTERLACED,
    };

static struct fb_fix_screeninfo vfb_fix = {
    .id =       "Sharp FB",
    .type =   FB_TYPE_PACKED_PIXELS,
    // --- FIX: line_length is 400 pixels / 8 bits/byte = 50 ---
    .line_length = LCDWIDTH / 8,
    .xpanstep = 0,
    .ypanstep = 0,
    .ywrapstep =    0,
    .visual =	FB_VISUAL_MONO10, // This is now consistent
    .accel =  FB_ACCEL_NONE,
};

static struct fb_ops vfb_ops = {
    .fb_read        = fb_sys_read,
    .fb_write       = fb_sys_write,
    .fb_fillrect    = sys_fillrect,
    .fb_copyarea    = sys_copyarea,
    .fb_imageblit   = sys_imageblit,
    .fb_mmap        = vfb_mmap, // Typos from previous versions are fixed
};

static struct task_struct *thread1;
static struct task_struct *fpsThread;
static struct task_struct *vcomToggleThread;

static int vfb_mmap(struct fb_info *info,
            struct vm_area_struct *vma)
{
    unsigned long start = vma->vm_start;
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    unsigned long page, pos;
    printk(KERN_CRIT "start %ld size %ld offset %ld", start, size, offset);

    if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
        return -EINVAL;
    if (size > info->fix.smem_len)
        return -EINVAL;
    if (offset > info->fix.smem_len - size)
        return -EINVAL;

    pos = (unsigned long)info->fix.smem_start + offset;

    while (size > 0) {
        page = vmalloc_to_pfn((void *)pos);
        if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED)) {
            return -EAGAIN;
        }
        start += PAGE_SIZE;
        pos += PAGE_SIZE;
        if (size > PAGE_SIZE)
            size -= PAGE_SIZE;
        else
            size = 0;
    }

    return 0;
}

void vfb_fillrect(struct fb_info *p, const struct fb_fillrect *region)
{
    printk(KERN_CRIT "from fillrect");
}

static void *rvmalloc(unsigned long size)
{
    void *mem;
    unsigned long adr;

    size = PAGE_ALIGN(size);
    mem = vmalloc_32(size);
    if (!mem)
        return NULL;

    memset(mem, 0, size); /* Clear the ram out, no junk to the user */
    adr = (unsigned long) mem;
    while (size > 0) {
        SetPageReserved(vmalloc_to_page((void *)adr));
        adr += PAGE_SIZE;
        size -= PAGE_SIZE;
    }

    return mem;
}

static void rvfree(void *mem, unsigned long size)
{
    unsigned long adr;

    if (!mem)
        return;

    adr = (unsigned long) mem;
    while ((long) size > 0) {
        ClearPageReserved(vmalloc_to_page((void *)adr));
        adr += PAGE_SIZE;
        size -= PAGE_SIZE;
    }
    vfree(mem);
}

void clearDisplay(void) {
    char buffer[2] = {clearByte, paddingByte};
    gpio_set_value(SCS, 1);

    spi_write(screen->spi, (const u8 *)buffer, 2);

    gpio_set_value(SCS, 0);
}

char reverseByte(char b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
  return b;
}

int vcomToggleFunction(void* v) 
{
    while (!kthread_should_stop()) 
    {
        msleep(50);
        vcomState = vcomState ? 0:1;
        gpio_set_value(VCOM, vcomState);
    }
    return 0;
}

int fpsThreadFunction(void* v)
{
    while (!kthread_should_stop()) 
    {
        msleep(5000);
    	printk(KERN_DEBUG "FPS sharp : %d\n", fpsCounter);
    	fpsCounter = 0;
    }
    return 0;
}

int thread_fn(void* v) 
{
    // --- REWRITE: This thread is now pure 1-bit-per-pixel ---
    int y;
    char hasChanged = 0;
    
    // Direct pointer to the 1bpp framebuffer memory
    char *fb_ptr = (char *)info->fix.smem_start;
    
    // SPI packet: 1 cmd + 1 line# + 50 data + 1 pad = 53 bytes
    char sendBuffer[53];

    printk(KERN_INFO "SHARP: Framebuffer reading thread_fn (1bpp) running\n");

    sendBuffer[0] = commandByte; // Command
    sendBuffer[52] = paddingByte; // Trailer

    clearDisplay();

    // Init screen to black (clear the cache and write it)
    memset(screenCache, 0, videomemorysize);
    for(y=0 ; y < LCDHEIGHT ; y++)
    {
        gpio_set_value(SCS, 1);
        sendBuffer[1] = reverseByte(y+1); // Line number
        // Copy 50 bytes of '0' from the cache into the send buffer
        memcpy(sendBuffer + 2, screenCache + (y * vfb_fix.line_length), vfb_fix.line_length); 
        spi_write(screen->spi, (const u8 *)sendBuffer, 53);
        gpio_set_value(SCS, 0);
    }

    // Main loop
    while (!kthread_should_stop()) 
    {
        msleep(50); // Polls the framebuffer ~20 times/sec

        for(y=0 ; y < LCDHEIGHT ; y++)
        {
            hasChanged = 0;
            
            // Pointers to the start of the line in FB and Cache
            char *fb_line_ptr = fb_ptr + (y * vfb_fix.line_length);
            char *cache_line_ptr = screenCache + (y * vfb_fix.line_length);

            // Compare the entire 50-byte line at once
            if (memcmp(fb_line_ptr, cache_line_ptr, vfb_fix.line_length) != 0)
            {
                hasChanged = 1;
            }

            if(hasChanged)
            {
                // 1. Update the cache
                memcpy(cache_line_ptr, fb_line_ptr, vfb_fix.line_length);
                
                // 2. Send the new line to the display
                gpio_set_value(SCS, 1);
                sendBuffer[1] = reverseByte(y+1); // Line number
                // Copy the 50 bytes from the (now updated) cache
                memcpy(sendBuffer + 2, cache_line_ptr, vfb_fix.line_length);
                spi_write(screen->spi, (const u8 *)(sendBuffer), 53);
                gpio_set_value(SCS, 0);
            }
        }
    }
    return 0;
}

static int sharp_probe(struct spi_device *spi)
{
    // --- "CANARY" MESSAGE ---
    printk(KERN_ERR "SHARP DRIVER: PROBE V4 RUNNING\n");

    char our_thread[] = "updateScreen";
    char thread_vcom[] = "vcom";
    char thread_fps[] = "fpsThread";
    int retval;

    screen = devm_kzalloc(&spi->dev, sizeof(*screen), GFP_KERNEL);
    if (!screen) {
        printk(KERN_ERR "SHARP: ERROR, devm_kzalloc failed\n");
        return -ENOMEM;
    }
    printk(KERN_DEBUG "SHARP: devm_kzalloc OK\n");

    spi->bits_per_word  = 8;
    spi->max_speed_hz   = 2000000;

    screen->spi = spi;

    spi_set_drvdata(spi, screen);

    // --- START NON-FRAMEBUFFER THREADS ---
    fpsThread = kthread_create(fpsThreadFunction,NULL,thread_fps);
    if((fpsThread))
    {
        wake_up_process(fpsThread);
    }

    vcomToggleThread = kthread_create(vcomToggleFunction,NULL,thread_vcom);
    if((vcomToggleThread))
    {
        wake_up_process(vcomToggleThread);
    }
    printk(KERN_DEBUG "SHARP: helper threads started\n");

    // --- SETUP GPIO ---
    retval = gpio_request(SCS, "SCS");
    if (retval) {
        printk(KERN_ERR "SHARP: ERROR, failed to request GPIO %d (SCS), code %d\n", SCS, retval);
        goto err_probe_early_exit;
    }
    gpio_direction_output(SCS, 0);

    retval = gpio_request(VCOM, "VCOM");
    if (retval) {
        printk(KERN_ERR "SHARP: ERROR, failed to request GPIO %d (VCOM), code %d\n", VCOM, retval);
        goto err_gpio_vcom;
    }
    gpio_direction_output(VCOM, 0);

    retval = gpio_request(DISP, "DISP");
    if (retval) {
        printk(KERN_ERR "SHARP: ERROR, failed to request GPIO %d (DISP), code %d\n", DISP, retval);
        goto err_gpio_disp;
    }
    gpio_direction_output(DISP, 1);
    printk(KERN_DEBUG "SHARP: GPIOs requested OK\n");

    // --- SCREEN (FRAMEBUFFER) PART ---
    
    retval = -ENOMEM;
    if (!(videomemory = rvmalloc(videomemorysize))) {
        printk(KERN_ERR "SHARP: ERROR, rvmalloc failed\n");
        goto err_gpio_free_all;
    }
    printk(KERN_DEBUG "SHARP: rvmalloc OK (size=%ld)\n", videomemorysize);

    // --- FIX: Allocate our screen cache ---
    screenCache = vzalloc(videomemorysize);
    if (!screenCache) {
        printk(KERN_ERR "SHARP: ERROR, vzalloc for screenCache failed\n");
        goto err; // Must free videomemory
    }
    printk(KERN_DEBUG "SHARP: screenCache allocated OK\n");

    memset(videomemory, 0, videomemorysize);

    info = framebuffer_alloc(sizeof(u32) * 256, &spi->dev);
    if (!info) {
        retval = -ENOMEM; // Set retval for the error log
        goto err; // Must free videomemory
    }
    printk(KERN_DEBUG "SHARP: framebuffer_alloc OK\n");

    // --- FIX: Remove __iomem cast ---
    // This is vmalloc'd system RAM, not hardware I/O registers.
    // The __iomem cast was likely breaking mmap caching.
    info->screen_base = (char *)videomemory;
    info->fbops = &vfb_ops;

    info->var = vfb_default;
    vfb_fix.smem_start = (unsigned long) videomemory;
    vfb_fix.smem_len = videomemorysize;
    info->fix = vfb_fix;
    info->par = NULL;
    info->flags = 0; // Fixed from FBINFO_FLAG_DEFAULT

    retval = fb_alloc_cmap(&info->cmap, 16, 0);
    if (retval < 0)
        goto err1; // Must release framebuffer
    printk(KERN_DEBUG "SHARP: fb_alloc_cmap OK\n");

    retval = register_framebuffer(info);
    if (retval < 0)
        goto err2; // Must dealloc cmap

    fb_info(info, "Virtual frame buffer device, using %ldK of video memory\n",
        videomemorysize >> 10);

    // --- START FRAMEBUFFER THREAD ---
    thread1 = kthread_create(thread_fn,NULL,our_thread);
    if((thread1))
    {
        wake_up_process(thread1);
    } 
    else 
    {
        printk(KERN_ERR "sharp: Failed to create updateScreen thread\n");
        retval = -ECHILD; 
        goto err2; 
    }
    
    return 0; // Success

// Error handling labels
err2:
    fb_dealloc_cmap(&info->cmap);
err1:
    framebuffer_release(info);
err:
    // --- FIX: Free the screen cache on error ---
    if (screenCache) vfree(screenCache);
    rvfree(videomemory, videomemorysize);
err_gpio_free_all:
    gpio_free(DISP);
err_gpio_disp:
    gpio_free(VCOM);
err_gpio_vcom:
    gpio_free(SCS);
err_probe_early_exit:
    // Stop the other threads we started
    if (fpsThread) kthread_stop(fpsThread);
    if (vcomToggleThread) kthread_stop(vcomToggleThread); // <-- *** TYPO 'kkthread_stop' FIXED ***

    printk(KERN_ERR "sharp: ERROR, probe failed with code %d\n", retval);
    return retval;
} 

static void sharp_remove(struct spi_device *spi)
{
        if (info) {
                unregister_framebuffer(info);
                fb_dealloc_cmap(&info->cmap);
                framebuffer_release(info);
        }
    // --- FIX: Free the screen cache ---
    if (screenCache) vfree(screenCache);
    
	kthread_stop(thread1);
	kthread_stop(fpsThread);
    kthread_stop(vcomToggleThread);
	printk(KERN_CRIT "out of screen module");
	return; // Changed from 'return 0'
}

static struct spi_driver sharp_driver = {
    .probe          = sharp_probe,
    .remove         = sharp_remove,
    .id_table       = sharp_id, // This line tells the driver to use the ID table
	.driver = {
		.name	= "sharp",
		.owner	= THIS_MODULE,
	},
};

module_spi_driver(sharp_driver);

MODULE_AUTHOR("Ael Gain <ael.gain@free.fr>");
MODULE_DESCRIPTION("Sharp memory lcd driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:sharp");

