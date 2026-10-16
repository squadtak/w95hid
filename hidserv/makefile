# HIDSERV.EXE (MSVC 5.0 environment; BASEDIR = 98DDK root; build ..\hiddll first)
#   nmake
!IF "$(BASEDIR)" == ""
!ERROR BASEDIR is not set (98DDK root)
!ENDIF
HIDINC = $(BASEDIR)\src\hid\inc

# Also runs from "build" (see ..\dirs): everything is rebuilt on its first pass,
# so that "build -cZ" compiles every file.
!IF "$(BUILD_PASS)" == "PASS1"
!IF [del /q *.obj *.exe *.res > nul 2>&1]
!ENDIF
!ENDIF

all: hidserv.exe

hidserv.res: hidserv.rc ..\hidver.h ..\verinfo.ver
	rc -r -fo hidserv.res hidserv.rc

# a Windows (not console) program: WinMain, no window of its own except a hidden one
hidserv.exe: hidserv.c hidserv.res ..\hiddll\hid.lib
	cl /nologo /MT /O2 /W3 -I$(HIDINC) hidserv.c hidserv.res ..\hiddll\hid.lib winmm.lib user32.lib kernel32.lib /link /subsystem:windows

clean:
	del *.obj *.exe *.res
