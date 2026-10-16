# w95hid

USB Human Interface Class driver for Windows 95 OSR2.x

with claude support.

<img width="800" height="600" alt="w95hid" src="https://github.com/user-attachments/assets/c38c77dc-9fd5-4f96-b9bf-26cb94c8a221" />

# Implemented
HID Keyboard

HID Mouse (limited wheel support, needs Microsoft IntelliPoint 4.0 driver, and check known issues)

Generic HID

# Build
MSVC5.0 + Win98 DDK

build -cZ

all generated files are copied to /build/w95hid

in win95, right click the three infs, install, restart, and then plugged in the usb device, oh-la-la!

Debug messages can be checked using dbgview

# Known Issues
* Microsoft's original usbsupp, as well as LoneCrusader and Rloew's openhci.sys and usbd.sys, cannot recognize devices with multiple endpoints. Only one endpoint per device is recognized.
All of the mice I own use two endpoints: one for the mouse button and one for the scroll wheel. Because of this, the scroll wheel doesn't work.

* Oracle VM VirtualBox 7.2.20 r175154 running Windows 95 OSR2.5+xusbsupp, the keyboard behaves abnormally. Keys sometimes get stuck, or the system freezes.
