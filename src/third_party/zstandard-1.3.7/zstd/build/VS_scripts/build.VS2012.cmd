@echo off

rem build 32-bit
call "%~p0%build.generic.cmd" VS2012 Win32 Release v110
rem build 64-bit
call "%~p0%build.generic.cmd" VS2012 x64 Release v110