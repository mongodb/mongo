@echo off

rem build 32-bit
call "%~p0%build.generic.cmd" VS2017Community Win32 Release v141

rem build 64-bit
call "%~p0%build.generic.cmd" VS2017Community x64 Release v141