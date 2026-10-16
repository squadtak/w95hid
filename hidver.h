/*
 * hidver.h - version information shared by every file of the Win95 USB HID port.
 * Included by the .rc file of each module (through verinfo.ver); change the
 * numbers here only.
 *
 * The VxDs (KBDHID, MOUHID, HIDEXT) get the same resource: the makefile compiles the
 * .rc (16-bit RC) and ADRC2VXD puts it into the LE file.  Their DDB version
 * (DECLARE_VIRTUAL_DEVICE name, 1, 0, ...) is separate and stays 1.0.
 */

/* the version, as text and as the four numbers of the binary resource (keep in step;
 * the 16-bit RC does not expand a macro made of other macros there, as in the DDK) */
#define HID_VER_STRING      "1.1"
#define HID_VER_NUMBERS     1,1,0,0

#define HID_VER_COMPANY     "Takuya Ohara a.k.a. squad"
/* product name and language (English US, code page 1252) as in the Win98 originals */
#define HID_VER_PRODUCT     "Microsoft(R) Windows(R) Operating System"
#define HID_VER_COPYRIGHT   "Copyright (C) Takuya Ohara 2026"
