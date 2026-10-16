@echo off
rem Host unit test of hidparse.c.  Needs BASEDIR (98DDK) and MSVC (vcvars32.bat).
cl /nologo /W3 /I. /I.. /I%BASEDIR%\src\hid\inc /Fehptest.exe hptest.c ..\hidparse.c
if errorlevel 1 goto end
hptest.exe
:end
