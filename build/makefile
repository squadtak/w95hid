# "build" collects the finished driver set into build\w95hid, next to the INF files
# kept there (W95HID.INF, W95HIDKB.INF, W95HIDMS.INF).  Take the w95hid folder to the
# Windows 95 machine.  This directory (SOURCES, makefile, obj) is only the build glue.
# It is the last entry of ..\dirs and runs in the compile pass of "build" (it has no
# link pass).  A file that has not been built (or whose build failed) is not copied;
# the old copies are deleted on the first pass.
#
#   HIDPARSE.SYS HIDCLASS.SYS HIDUSB.SYS   copied by the makefile.inc of their own
#                                          directory, after the link pass
#   HIDEXT.VXD KBDHID.VXD MOUHID.VXD       <dir>\   (copied below)
#   HID.DLL, HIDTEST.EXE, HIDSERV.EXE      <dir>\   (copied below)

OUT = w95hid

!IF "$(BUILD_PASS)" == "PASS1"
!IF [del /q $(OUT)\*.sys $(OUT)\*.vxd $(OUT)\*.dll $(OUT)\*.exe > nul 2>&1]
!ENDIF
!ENDIF

all:
	@if exist ..\hidext\hidext.vxd copy /y ..\hidext\hidext.vxd $(OUT) > nul
	@if exist ..\kbdhid\kbdhid.vxd copy /y ..\kbdhid\kbdhid.vxd $(OUT) > nul
	@if exist ..\mouhid\mouhid.vxd copy /y ..\mouhid\mouhid.vxd $(OUT) > nul
	@if exist ..\hiddll\hid.dll copy /y ..\hiddll\hid.dll $(OUT) > nul
	@if exist ..\hidtest\hidtest.exe copy /y ..\hidtest\hidtest.exe $(OUT) > nul
	@if exist ..\hidserv\hidserv.exe copy /y ..\hidserv\hidserv.exe $(OUT) > nul

clean:
	del $(OUT)\*.sys $(OUT)\*.vxd $(OUT)\*.dll $(OUT)\*.exe
