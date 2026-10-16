# KBDHID.VXD (MSVC 5.0 environment with ML.EXE and CL.EXE; BASEDIR = 98DDK root)
#   nmake
#
#   kbdhida.obj  kbdhid.asm  - DDB, control procedure, service wrappers
#   kbdhidc.obj  kbdhid.c    - the keyboard client
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

all: KBDHID.VXD

kbdhida.obj: kbdhid.asm
	ml $(AFLAGS) -Fo$@ kbdhid.asm

kbdhidc.obj: kbdhid.c ..\hidparse\hpvxd.h
	cl $(CFLAGS) -Fo$@ kbdhid.c

hidparse.obj: ..\hidparse\hidparse.c ..\hidparse\hpport.h ..\hidparse\hpvxd.h
	cl $(CFLAGS) -DHP_VXD -Fo$@ ..\hidparse\hidparse.c

kbdhid.res: kbdhid.rc ..\hidver.h ..\verinfo.ver
	$(RC16) -r -fo kbdhid.res kbdhid.rc

# The version resource goes into the VxD afterwards (ADRC2VXD <vxd> <res>, RC16 above).
KBDHID.VXD: kbdhida.obj kbdhidc.obj hidparse.obj kbdhid.def kbdhid.res
	link -MACHINE:i386 -DEBUG:NONE -PDB:NONE -ignore:4078 -ignore:4039 -ignore:4049 \
	     -DEF:kbdhid.def -OUT:KBDHID.VXD -MAP:KBDHID.MAP -VXD \
	     kbdhida.obj kbdhidc.obj hidparse.obj
	adrc2vxd KBDHID.VXD kbdhid.res

clean:
	del *.obj *.vxd *.map *.exp *.lib *.res
