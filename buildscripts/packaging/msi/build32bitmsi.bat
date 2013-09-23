@ECHO OFF

SET VERSION=2.4.0
SET BINDIR=..\..\..\build\win32\normal\mongo
SET CLIENTLIBDIR=..\..\..\build\win32\normal\client_build
SET LICENSEDIR=..\..\..\distsrc
SET CLIENTHEADERDIR=..\..\..\build\win32\normal\client_build\include
SET WIXBINDIR=C:\Program Files (x86)\WiX Toolset v3.7\bin

SET PLATFORM=x86
SET GENERATEDWXSDIR=.\wxs
SET EDITION=Standard
SET CONFIGURATION=Release
SET OUTPUTOBJDIR=obj\%CONFIGURATION%\%PLATFORM%\
SET OUTPUTBINDIR=bin\%CONFIGURATION%\%PLATFORM%\
SET PROJECTDIR=C:\git\sridharn\mongo\buildscripts\packaging\msi\
SET TARGETNAME=MongoDB_%VERSION%_%PLATFORM%_%EDITION%

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
    IF "%1"=="-clientheaderdir" (
        SET CLIENTHEADERDIR=%2
        SHIFT
    )
    IF "%1"=="-wixbindir" (
        SET WIXBINDIR=%2
        SHIFT
    )
    IF "%1"=="-generatedwxsdir" (
        SET GENERATEDWXSDIR=%2
        SHIFT
    )
    SHIFT
    GOTO :loop
)

REM ECHO Building msi for version %VERSION% with binaries from %BINDIR% and license files from %LICENSEDIR%
REM %WINDIR%\Microsoft.NET\Framework64\v4.0.30319\msbuild /p:Configuration=Release;Version=%VERSION%;License=%LICENSEDIR%;
REM Source=%BINDIR%;ClientSource=%CLIENTLIBDIR%;ClientHeaderSource=%CLIENTHEADERDIR% MongoDB.wixproj

ECHO Generating %GENERATEDWXSDIR%\DriverInclude.wxs from sources at %CLIENTHEADERDIR%
"%WIXBINDIR%\heat.exe" dir %CLIENTHEADERDIR% -gg -g1 -frag -cg cg_DriverHeaders -nologo -directoryid -out %GENERATEDWXSDIR%\DriverInclude.wxs -dr Header -srd -var var.ClientHeaderSource

ECHO Compiling wxs files to obj
"%WIXBINDIR%\candle.exe" -wx^
    -dMongoDBVersion=%VERSION%^
    -dLicenseSource=%LICENSEDIR%^
    -dBinarySource=%BINDIR%^
    -dEdition=%EDITION%^
    -d"ProductId=*"^
    -dUpgradeCode=867C1D1D-2040-4E90-B04E-1158F9CBDE96^
    -dClientSource=%CLIENTLIBDIR%^
    -dClientHeaderSource=%CLIENTHEADERDIR%^
    -dConfiguration=%CONFIGURATION%^
    -dOutDir=%OUTPUTBINDIR%^
    -dPlatform=%PLATFORM%^
    -dProjectDir=%PROJECTDIR%^
    -dProjectExt=.wixproj^
    -dProjectFileName=MongoDB.wixproj^
    -dProjectName=MongoDB^
    -dProjectPath=%PROJECTDIR%\MongoDB.wixproj^
    -dTargetDir=%OUTPUTBINDIR%^
    -dTargetExt=.msi^
    -dTargetFileName=%TARGETNAME%.msi^
    -dTargetName=%TARGETNAME%^
    -dTargetPath=%OUTPUTBINDIR%\%TARGETNAME%.msi^
    -out %OUTPUTOBJDIR%^
    -arch %PLATFORM%^
    -ext "%WIXBINDIR%\WixUIExtension.dll"^
    wxs\BinaryFragment.wxs wxs\FeatureFragment.wxs wxs\LicensingFragment.wxs wxs\Installer.wxs %GENERATEDWXSDIR%\DriverInclude.wxs

ECHO Linking to msi
"%WIXBINDIR%\Light.exe"^
    -out %OUTPUTBINDIR%\%TARGETNAME%.msi^
    -pdbout %OUTPUTBINDIR%\%TARGETNAME%.wixpdb^
    -wx -cultures:null^
    -ext "%WIXBINDIR%\WixUIExtension.dll"^
    -contentsfile %OUTPUTOBJDIR%\MongoDB.wixproj.BindContentsFileListnull.txt^
    -outputsfile %OUTPUTOBJDIR%\MongoDB.wixproj.BindOutputsFileListnull.txt^
    -builtoutputsfile %OUTPUTOBJDIR%\MongoDB.wixproj.BindBuiltOutputsFileListnull.txt^
    -wixprojectfile %PROJECTDIR%\MongoDB.wixproj^
    %OUTPUTOBJDIR%\BinaryFragment.wixobj %OUTPUTOBJDIR%\FeatureFragment.wixobj^
    %OUTPUTOBJDIR%\\LicensingFragment.wixobj %OUTPUTOBJDIR%\Installer.wixobj^
    %OUTPUTOBJDIR%\DriverInclude.wixobj
