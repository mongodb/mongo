@echo off

rem build 32-bit
call "%~p0%build.generic.cmd" VS2013 Win32 Release v120

rem build 64-bit
call "%~p0%build.generic.cmd" VS2013 x64 Release v120