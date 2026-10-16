# HIDTEST.EXE (MSVC 5.0 environment; BASEDIR = 98DDK root; build ..\hiddll first)
#   nmake
!IF "$(BASEDIR)" == ""
!ERROR BASEDIR is not set (98DDK root)
!ENDIF
HIDINC = $(BASEDIR)\src\hid\inc

# Also runs from "build" (see ..\dirs): everything is rebuilt on its first pass,
# so that "build -cZ" compiles every file.
!IF "$(BUILD_PASS)" == "PASS1"
!IF [del /q *.obj *.exe > nul 2>&1]
!ENDIF
!ENDIF

all: hidtest.exe

hidtest.exe: hidtest.c ..\hiddll\hid.lib
	cl /nologo /MT /O2 /W3 -I$(HIDINC) hidtest.c ..\hiddll\hid.lib user32.lib kernel32.lib

clean:
	del *.obj *.exe
