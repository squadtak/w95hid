# w95hid

USB Human Interface Class driver for Windows 95 OSR2.x

with claude support.

<img width="800" height="600" alt="w95hid" src="https://github.com/user-attachments/assets/c38c77dc-9fd5-4f96-b9bf-26cb94c8a221" />

# Implemented
HID Keyboard

HID Mouse (with limited wheel support)

Generic HID

# Build
MSVC5.0 + Win98 DDK

build -cZ

all the created files copied on /build/w95hid

in win95, right click the three inf, install, restart, and then plugged in the usb device, oh-la-la!

Debug messages can be checked using dbgview

# Known Issues
Microsoft's original usbsupp, as well as LoneCrusader and Rloew's openhci.sys and usbd.sys, cannot recognize devices with multiple endpoints. Only one endpoint per device is recognized.

All of the mice I own use two endpoints: one for the mouse button and one for the scroll wheel. Because of this, the scroll wheel doesn't work.
