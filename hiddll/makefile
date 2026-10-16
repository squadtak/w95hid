# HID.DLL (MSVC 5.0 environment; BASEDIR = 98DDK root)
#   nmake
#
#   hiddll.obj   hiddll.c               - HidD_* and the Win95 extensions
#   hidparse.obj ..\hidparse\hidparse.c - HidP_* (the same source as HIDPARSE.SYS)
# The static C runtime (-MT) is used: Windows 95 has no MSVCRT.DLL of its own.
!IF "$(BASEDIR)" == ""
!ERROR BASEDIR is not set (98DDK root)
!ENDIF
HIDINC = $(BASEDIR)\src\hid\inc

# Also runs from "build" (see ..\dirs): everything is rebuilt on its first pass,
# so that "build -cZ" compiles every file.
!IF "$(BUILD_PASS)" == "PASS1"
!IF [del /q *.obj *.dll *.exp *.lib *.map *.res > nul 2>&1]
!ENDIF
!ENDIF

CFLAGS = -nologo -MT -O2 -W3 -D_X86_ -I..\hidparse\host -I$(HIDINC)

all: HID.DLL

hiddll.obj: hiddll.c ..\hidpriv.h
	cl $(CFLAGS) -c -Fo$@ hiddll.c

# the parser is plain C (host mode: its own tiny type shims and the C runtime's malloc)
hidparse.obj: ..\hidparse\hidparse.c ..\hidparse\hpport.h
	cl $(CFLAGS) -I..\hidparse -c -Fo$@ ..\hidparse\hidparse.c

hid.res: hid.rc ..\hidver.h ..\verinfo.ver
	rc -r -fo hid.res hid.rc

HID.DLL: hiddll.obj hidparse.obj hid.res hid.def
	link -nologo -DLL -MACHINE:i386 -DEF:hid.def -OUT:HID.DLL -IMPLIB:HID.LIB \
	     hiddll.obj hidparse.obj hid.res kernel32.lib user32.lib

clean:
	del *.obj *.dll *.exp *.lib *.map *.res
