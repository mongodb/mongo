@echo off

rem build 32-bit
call "%~p0%build.generic.cmd" VS2010 Win32 Release v100

rem build 64-bit
call "%~p0%build.generic.cmd" VS2010 x64 Release v100