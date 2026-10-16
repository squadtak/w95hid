# MOUHID.VXD (MSVC 5.0 environment with ML.EXE and CL.EXE; BASEDIR = 98DDK root)
#   nmake
#
#   mouhida.obj  mouhid.asm  - DDB, control procedure, service wrappers
#   mouhidc.obj  mouhid.c    - the mouse client
#   hidparse.obj ..\hidparse\hidparse.c  - HidP_* (the same source as HIDPARSE.SYS)
!IF "$(BASEDIR)" == ""
!ERROR BASEDIR is not set (98DDK root)
!ENDIF

# Also runs from "build" (see ..\dirs): everything is rebuilt on its first pass,
# so that "build -cZ" compiles every file.
!IF "$(BUILD_PASS)" == "PASS1"
!IF [del /q *.obj *.vxd *.map *.exp *.lib *.res > nul 2>&1]
!ENDIF
!ENDIF
INC32 = $(BASEDIR)\inc\win98
# ADRC2VXD reads the 16-bit .res format, so the version resource is compiled with the
# 16-bit RC of the DDK (not the 32-bit one of MSVC); it opens include files by 8.3 names.
RC16 = $(BASEDIR)\bin\win98\bin16\rc.exe
HIDINC = $(BASEDIR)\src\hid\inc

# -DWIN40COMPAT: DDB SDK version 0400h (the OSR2.1 VMM refuses 040Ah)
AFLAGS = -coff -DBLD_COFF -DIS_32 -W2 -Zd -c -Cx -DMASM6 -DWIN40COMPAT -DDEBLEVEL=0 -I$(INC32)
# -Zl: no CRT   -Gs: no stack probes   -Oi-: memset/memcpy are our own functions
CFLAGS = -nologo -c -Zl -Gs -Ox -Oi- -W3 -DIS_32 -DWIN40COMPAT -D_X86_ -I..\hidparse -I$(HIDINC)

all: MOUHID.VXD

mouhida.obj: mouhid.asm
	ml $(AFLAGS) -Fo$@ mouhid.asm

mouhidc.obj: mouhid.c ..\hidparse\hpvxd.h
	cl $(CFLAGS) -Fo$@ mouhid.c

hidparse.obj: ..\hidparse\hidparse.c ..\hidparse\hpport.h ..\hidparse\hpvxd.h
	cl $(CFLAGS) -DHP_VXD -Fo$@ ..\hidparse\hidparse.c

mouhid.res: mouhid.rc ..\hidver.h ..\verinfo.ver
	$(RC16) -r -fo mouhid.res mouhid.rc

# The version resource goes into the VxD afterwards (ADRC2VXD <vxd> <res>, RC16 above).
MOUHID.VXD: mouhida.obj mouhidc.obj hidparse.obj mouhid.def mouhid.res
	link -MACHINE:i386 -DEBUG:NONE -PDB:NONE -ignore:4078 -ignore:4039 -ignore:4049 \
	     -DEF:mouhid.def -OUT:MOUHID.VXD -MAP:MOUHID.MAP -VXD \
	     mouhida.obj mouhidc.obj hidparse.obj
	adrc2vxd MOUHID.VXD mouhid.res

clean:
	del *.obj *.vxd *.map *.exp *.lib *.res
