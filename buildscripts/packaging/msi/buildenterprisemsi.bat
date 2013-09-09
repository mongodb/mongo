@ECHO OFF
SET VERSION=2.6.0
SET BINDIR=..\..\..\build\win32\64\dynamic-windows\extrapathdyn_c__Utils_sasl_c__Utils_snmp_c__Utils_ssl\release\ssl\mongo
SET CLIENTLIBDIR=..\..\..\build\win32\64\dynamic-windows\extrapathdyn_c__Utils_sasl_c__Utils_snmp_c__Utils_ssl\release\ssl\client_build
SET LICENSEDIR=..\..\..\distsrc
SET EDITION=Enterprise
SET FLAVOR=2008R2Plus
SET SASLDIR=..\..\..\..\..\..\Utils\sasl\bin
SET OPENSSLDIR=..\..\..\..\..\..\Utils\ssl\bin
SET SNMPDIR=..\..\..\..\..\..\Utils\snmp\bin

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
    IF "%1"=="-sasldir" (
        SET SASLDIR=%2
        SHIFT
    )
    IF "%1"=="-openssldir" (
        SET OPENSSLDIR=%2
        SHIFT
    )
    IF "%1"=="-snmpdir" (
        SET SNMPDIR=%2
        SHIFT
    )
    IF "%1"=="-clientlibdir" (
        SET CLIENTLIBDIR=%2
        SHIFT
    )
    SHIFT
    GOTO :loop
)

ECHO Building enterprise msi for version %VERSION% with binaries from %BINDIR%, sasl from %SASLDIR%, ssl from %OPENSSLDIR%, snmp from %SNMPDIR% and license files from %LICENSEDIR%

%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\msbuild /p:Configuration=Release;Version=%VERSION%;License=%LICENSEDIR%;Source=%BINDIR%;SaslSource=%SASLDIR%;SnmpSource=%SNMPDIR%;SslSource=%OPENSSLDIR%;Edition=%EDITION%;Flavor=%FLAVOR%;ClientSource=%CLIENTLIBDIR% MongoDB_64.wixproj