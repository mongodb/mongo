@echo off

IF "%1%" == "" GOTO display_help

SETLOCAL

SET msbuild_version=%1

SET msbuild_platform=%2
IF "%msbuild_platform%" == "" SET msbuild_platform=x64

SET msbuild_configuration=%3
IF "%msbuild_configuration%" == "" SET msbuild_configuration=Release

SET msbuild_toolset=%4

GOTO build

:display_help

echo Syntax: build.generic.cmd msbuild_version msbuild_platform msbuild_configuration msbuild_toolset
echo   msbuild_version:          VS installed version (VS2012, VS2013, VS2015, VS2017, ...)
echo   msbuild_platform:         Platform (x64 or Win32)
echo   msbuild_configuration:    VS configuration (Release or Debug)
echo   msbuild_toolset:          Platform Toolset (v100, v110, v120, v140, v141)

EXIT /B 1

:build

SET msbuild="%windir%\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
SET msbuild_vs2017community="%programfiles(x86)%\Microsoft Visual Studio\2017\Community\MSBuild\15.0\Bin\MSBuild.exe"
SET msbuild_vs2017professional="%programfiles(x86)%\Microsoft Visual Studio\2017\Professional\MSBuild\15.0\Bin\MSBuild.exe"
SET msbuild_vs2017enterprise="%programfiles(x86)%\Microsoft Visual Studio\2017\Enterprise\MSBuild\15.0\Bin\MSBuild.exe"
IF %msbuild_version% == VS2013 SET msbuild="%programfiles(x86)%\MSBuild\12.0\Bin\MSBuild.exe"
IF %msbuild_version% == VS2015 SET msbuild="%programfiles(x86)%\MSBuild\14.0\Bin\MSBuild.exe"
IF %msbuild_version% == VS2017Community SET msbuild=%msbuild_vs2017community%
IF %msbuild_version% == VS2017Professional SET msbuild=%msbuild_vs2017professional%
IF %msbuild_version% == VS2017Enterprise SET msbuild=%msbuild_vs2017enterprise%
IF %msbuild_version% == VS2017 (
	IF EXIST %msbuild_vs2017community% SET msbuild=%msbuild_vs2017community%
	IF EXIST %msbuild_vs2017professional% SET msbuild=%msbuild_vs2017professional%
	IF EXIST %msbuild_vs2017enterprise% SET msbuild=%msbuild_vs2017enterprise%
)

SET project="%~p0\..\VS2010\zstd.sln"

SET msbuild_params=/verbosity:minimal /nologo /t:Clean,Build /p:Platform=%msbuild_platform% /p:Configuration=%msbuild_configuration%
IF NOT "%msbuild_toolset%" == "" SET msbuild_params=%msbuild_params% /p:PlatformToolset=%msbuild_toolset%

SET output=%~p0%bin
SET output="%output%/%msbuild_configuration%/%msbuild_platform%/"
SET msbuild_params=%msbuild_params% /p:OutDir=%output%

echo ### Building %msbuild_version% project for %msbuild_configuration% %msbuild_platform% (%msbuild_toolset%)...
echo ### Build Params: %msbuild_params%

%msbuild% %project% %msbuild_params%
IF ERRORLEVEL 1 EXIT /B 1
echo # Success
echo # OutDir: %output%
echo #
