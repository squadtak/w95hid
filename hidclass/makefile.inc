# NTTARGETFILES runs after the link pass has linked the driver (and, harmlessly, in the
# compile pass before it: then the file is missing or still the previous build).
CopyToDriver:
	@if exist obj\i386\free\hidclass.sys copy /y obj\i386\free\hidclass.sys ..\build\w95hid
