# Sharp Memory LCD Kernel Driver

**_This is a rewrite of the driver for newer (6.12) kernel versions._**

The original links were removed because the original writer and their website is gone.

This driver is for the LS027B7DH01. It *should* work with other Sharp Mem LCD displays by modifying all 400/240 references with the correct dimensions for your screen.

## Hookup Guide
Connect the following pins:

Display | RasPi (IO numbers)
------- | ---------
VIN     | 3.3V      
3V3     | N/C       
GND     | GND       
SCLK    | 11 (SCLK) 
MOSI    | 10 (MOSI) 
CS      | 23        
EXTMD   | 3.3V      
DISP    | 24        
EXTIN   | 25        

## Compile/Install the driver
### Requirements
Verify that you have the linux kernel headers for your platform. For the RasPi these can be obtained by:
```
sudo apt install linux-headers-$(uname -r)
```

Install necessary build tools (assuming make/gcc is already there):
```
sudo apt install git device-tree-compiler build-essential
```

### Cloning
Clone the repository and then move into the directory:
```
mkdir driver_dir
cd driver_dir
git clone https://github.com/snikolaj/Sharp-Memory-LCD-Kernel-Driver.git
cd Sharp-Memory-LCD-Kernel-Driver
```

### Compilation and install
To compile the driver, run:
```
make
```

To install the driver, run:
```
sudo make modules_install
sudo depmod -a
sudo update-initramfs -u
```

If you want the module to load at boot you'll need to add it to the /etc/modules file, like:
```
...
# This file contains...
# at boot time...
sharp
```

## Compile/Install the Device Tree Overlay
The included sharp.dts file is for the Raspberry Pi Zero W. To compile it, run:
```
dtc -@ -I dts -O dtb -o sharp.dtbo sharp.dts
```

To load it at runtime, copy it to /boot/firmware/overlays (changed from /boot/overlays in old versions):
```
sudo cp sharp.dtbo /boot/firmware/overlays/
```

And then add the following line to /boot/firmware/config.txt:
```
dtoverlay=sharp
```

## Console on Display
If you want the boot console to show up on the display, you'll need to append `fbcon=map:0` (if this is your only display) or `fbcon=map:1` (if you're using HDMI as well as the Sharp) to /boot/firmware/cmdline.txt after *rootwait*, like:
```
... rootwait ... fbcon=map:0
```

To make sure the console fits on screen, uncomment the following lines in /boot/firmware/config.txt and set the resolution appropriately:
```
framebuffer_width=400
framebuffer_height=240
```

## Verification
To check for errors:

### Check Module: See if the sharp module is loaded.
```
lsmod | grep sharp
```

### Check Kernel Log: Look for messages related to the driver loading and probing. Check for errors like the GPIO request failures seen earlier.
```
dmesg | grep -i sharp
```
(Look for SHARP DRIVER: PROBE V4 RUNNING and successful messages like GPIOs requested OK, framebuffer_alloc OK).

### Check Framebuffer Device: Verify that the framebuffer device (/dev/fb0 or potentially /dev/fb1) has been created. The history shows /dev/fb0 was created.
```
ls /dev/fb*
```

### Basic Test: Try writing random data (this will make the screen show static if successful).
```
sudo cat /dev/urandom > /dev/fb0 # Or /dev/fb1 if it exists
```

# Test script
This project includes a Python test script. Run it with:
```python
sudo apt install python3-pygame python3-pil python3-gpiozero
chmod +x gui.py
sudo ./gui.py
```
