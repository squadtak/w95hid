# HIDEXT.VXD (MSVC 5.0 environment with ML.EXE and CL.EXE; BASEDIR = 98DDK root)
#   nmake
#
#   hidexta.obj  hidext.asm  - DDB, control procedure, NTKERN service wrappers
#   hidextc.obj  hidext.c    - the shutdown notice
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

# -DWIN40COMPAT: DDB SDK version 0400h (the OSR2.1 VMM refuses 040Ah)
AFLAGS = -coff -DBLD_COFF -DIS_32 -W2 -Zd -c -Cx -DMASM6 -DWIN40COMPAT -DDEBLEVEL=0 -I$(INC32)
# -Zl: no CRT   -Gs: no stack probes   -Oi-: no intrinsics
CFLAGS = -nologo -c -Zl -Gs -Ox -Oi- -W3 -DIS_32 -DWIN40COMPAT -D_X86_

all: HIDEXT.VXD

hidexta.obj: hidext.asm
	ml $(AFLAGS) -Fo$@ hidext.asm

hidextc.obj: hidext.c ..\hidtrace.h
	cl $(CFLAGS) -Fo$@ hidext.c

hidext.res: hidext.rc ..\hidver.h ..\verinfo.ver
	$(RC16) -r -fo hidext.res hidext.rc

# The version resource goes into the VxD afterwards (ADRC2VXD <vxd> <res>, RC16 above).
HIDEXT.VXD: hidexta.obj hidextc.obj hidext.def hidext.res
	link -MACHINE:i386 -DEBUG:NONE -PDB:NONE -ignore:4078 -ignore:4039 -ignore:4049 \
	     -DEF:hidext.def -OUT:HIDEXT.VXD -MAP:HIDEXT.MAP -VXD \
	     hidexta.obj hidextc.obj
	adrc2vxd HIDEXT.VXD hidext.res

clean:
	del *.obj *.vxd *.map *.exp *.lib *.res
