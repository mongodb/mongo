@ECHO OFF
SET VERSION=2.4.0
SET BINDIR=..\..\..\build\win32\64\mongo
SET CLIENTLIBDIR=..\..\..\build\win32\64\client_build
SET LICENSEDIR=..\..\..\distsrc
SET EDITION=Standard
SET FLAVOR=2008R2Plus
:loop
IF NOT "%1"=="" (
    IF "%1"=="-version" (
        SET VERSION=%2
        SHIFT
    )
    IF "%1"=="-bindir" (
        SET BINDIR=%2
        SHIFT
    )
    IF "%1"=="-licensedir" (
        SET LICENSEDIR=%2
        SHIFT
    )
    IF "%1"=="-clientlibdir" (
        SET CLIENTLIBDIR=%2
        SHIFT
    )
    SHIFT
    GOTO :loop
)

ECHO Building msi for version %VERSION% with binaries from %BINDIR% and license files from %LICENSEDIR%
%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\msbuild /p:Configuration=Release;Version=%VERSION%;License=%LICENSEDIR%;Source=%BINDIR%;Edition=%EDITION%;Flavor=%FLAVOR%;ClientSource=%CLIENTLIBDIR% MongoDB_64.wixproj