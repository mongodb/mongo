# Microsoft Developer Studio Generated NMAKE File, Format Version 4.20
# ** DO NOT EDIT **

# TARGTYPE "Win32 (x86) Console Application" 0x0103
# TARGTYPE "Win32 (x86) Dynamic-Link Library" 0x0102
# TARGTYPE "Win32 (x86) Static Library" 0x0104

!IF "$(CFG)" == ""
CFG=jsshell - Win32 Debug
!MESSAGE No configuration specified.  Defaulting to jsshell - Win32 Debug.
!ENDIF 

!IF "$(CFG)" != "js - Win32 Release" && "$(CFG)" != "js - Win32 Debug" &&\
 "$(CFG)" != "jsshell - Win32 Release" && "$(CFG)" != "jsshell - Win32 Debug" &&\
 "$(CFG)" != "jskwgen - Win32 Release" && "$(CFG)" != "jskwgen - Win32 Debug" &&\
 "$(CFG)" != "fdlibm - Win32 Release" && "$(CFG)" != "fdlibm - Win32 Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE on this makefile
!MESSAGE by defining the macro CFG on the command line.  For example:
!MESSAGE 
!MESSAGE NMAKE /f "js.mak" CFG="jsshell - Win32 Debug"
!MESSAGE 
!MESSAGE Possible choices for configuration are:
!MESSAGE 
!MESSAGE "js - Win32 Release" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE "js - Win32 Debug" (based on "Win32 (x86) Dynamic-Link Library")
!MESSAGE "jsshell - Win32 Release" (based on "Win32 (x86) Console Application")
!MESSAGE "jsshell - Win32 Debug" (based on "Win32 (x86) Console Application")
!MESSAGE "jskwgen - Win32 Release" (based on "Win32 (x86) Static Library")
!MESSAGE "jskwgen - Win32 Debug" (based on "Win32 (x86) Static Library")
!MESSAGE "fdlibm - Win32 Release" (based on "Win32 (x86) Static Library")
!MESSAGE "fdlibm - Win32 Debug" (based on "Win32 (x86) Static Library")
!MESSAGE 
!ERROR An invalid configuration is specified.
!ENDIF 

!IF "$(OS)" == "Windows_NT"
NULL=
!ELSE 
NULL=nul
!ENDIF 
################################################################################
# Begin Project
# PROP Target_Last_Scanned "jsshell - Win32 Debug"

!IF  "$(CFG)" == "js - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "js___Wi1"
# PROP BASE Intermediate_Dir "js___Wi1"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Target_Dir ""
OUTDIR=.\Release
INTDIR=.\Release

ALL : "fdlibm - Win32 Release" "jskwgen - Win32 Release" "$(OUTDIR)\js32.dll"

CLEAN : 
	-@erase "$(INTDIR)\jsapi.obj"
	-@erase "$(INTDIR)\jsarena.obj"
	-@erase "$(INTDIR)\jsarray.obj"
	-@erase "$(INTDIR)\jsatom.obj"
	-@erase "$(INTDIR)\jsbool.obj"
	-@erase "$(INTDIR)\jscntxt.obj"
	-@erase "$(INTDIR)\jsdate.obj"
	-@erase "$(INTDIR)\jsdbgapi.obj"
	-@erase "$(INTDIR)\jsdhash.obj"
	-@erase "$(INTDIR)\jsdtoa.obj"
	-@erase "$(INTDIR)\jsemit.obj"
	-@erase "$(INTDIR)\jsexn.obj"
	-@erase "$(INTDIR)\jsfun.obj"
	-@erase "$(INTDIR)\jsgc.obj"
	-@erase "$(INTDIR)\jshash.obj"
	-@erase "$(INTDIR)\jsinterp.obj"
	-@erase "$(INTDIR)\jslock.obj"
	-@erase "$(INTDIR)\jslog2.obj"
	-@erase "$(INTDIR)\jslong.obj"
	-@erase "$(INTDIR)\jsmath.obj"
	-@erase "$(INTDIR)\jsnum.obj"
	-@erase "$(INTDIR)\jsobj.obj"
	-@erase "$(INTDIR)\jsopcode.obj"
	-@erase "$(INTDIR)\jsparse.obj"
	-@erase "$(INTDIR)\jsprf.obj"
	-@erase "$(INTDIR)\jsregexp.obj"
	-@erase "$(INTDIR)\jsscan.obj"
	-@erase "$(INTDIR)\jsscope.obj"
	-@erase "$(INTDIR)\jsscript.obj"
	-@erase "$(INTDIR)\jsstr.obj"
	-@erase "$(INTDIR)\jsutil.obj"
	-@erase "$(INTDIR)\jsxdrapi.obj"
	-@erase "$(INTDIR)\jsxml.obj"
	-@erase "$(INTDIR)\prmjtime.obj"
	-@erase "$(INTDIR)\js.pch"
	-@erase "$(INTDIR)\jsautokw.h"
	-@erase "$(OUTDIR)\js32.dll"
	-@erase "$(OUTDIR)\js32.exp"
	-@erase "$(OUTDIR)\js32.lib"
	-@$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="fdlibm - Win32 Release" clean
	-@$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="jskwgen - Win32 Release" clean

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /MT /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D _X86_=1 /D "_WINDOWS" /YX /c
# ADD CPP /nologo /MD /W3 /GX /O2 /D "NDEBUG" /D _X86_=1 /D "_WINDOWS" /D "WIN32" /D "XP_WIN" /D "JSFILE" /D "EXPORT_JS_API" /I"$(INTDIR)" /YX /c
CPP_PROJ=/nologo /MD /W3 /GX /O2 /D "NDEBUG" /D _X86_=1 /D "_WINDOWS" /D "WIN32" /D\
 "XP_WIN" /D "JSFILE" /D "EXPORT_JS_API" /Fp"$(INTDIR)/js.pch" /I"$(INTDIR)" /YX\
 /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\Release/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

MTL=mktyplib.exe
# ADD BASE MTL /nologo /D "NDEBUG" /win32
# ADD MTL /nologo /D "NDEBUG" /win32
MTL_PROJ=/nologo /D "NDEBUG" /win32 
RSC=rc.exe
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/js.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:windows /dll /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:windows /dll /machine:I386 /out:"Release/js32.dll"
# SUBTRACT LINK32 /nodefaultlib
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /nologo /subsystem:windows /dll /incremental:no\
 /pdb:"$(OUTDIR)/js32.pdb" /machine:I386 /out:"$(OUTDIR)/js32.dll"\
 /implib:"$(OUTDIR)/js32.lib" /opt:ref /opt:noicf
LINK32_OBJS= \
	"$(INTDIR)\jsapi.obj" \
	"$(INTDIR)\jsarena.obj" \
	"$(INTDIR)\jsarray.obj" \
	"$(INTDIR)\jsatom.obj" \
	"$(INTDIR)\jsbool.obj" \
	"$(INTDIR)\jscntxt.obj" \
	"$(INTDIR)\jsdate.obj" \
	"$(INTDIR)\jsdbgapi.obj" \
	"$(INTDIR)\jsdhash.obj" \
	"$(INTDIR)\jsdtoa.obj" \
	"$(INTDIR)\jsemit.obj" \
	"$(INTDIR)\jsexn.obj" \
	"$(INTDIR)\jsfun.obj" \
	"$(INTDIR)\jsgc.obj" \
	"$(INTDIR)\jshash.obj" \
	"$(INTDIR)\jsinterp.obj" \
	"$(INTDIR)\jslock.obj" \
	"$(INTDIR)\jslog2.obj" \
	"$(INTDIR)\jslong.obj" \
	"$(INTDIR)\jsmath.obj" \
	"$(INTDIR)\jsnum.obj" \
	"$(INTDIR)\jsobj.obj" \
	"$(INTDIR)\jsopcode.obj" \
	"$(INTDIR)\jsparse.obj" \
	"$(INTDIR)\jsprf.obj" \
	"$(INTDIR)\jsregexp.obj" \
	"$(INTDIR)\jsscan.obj" \
	"$(INTDIR)\jsscope.obj" \
	"$(INTDIR)\jsscript.obj" \
	"$(INTDIR)\jsstr.obj" \
	"$(INTDIR)\jsutil.obj" \
	"$(INTDIR)\jsxdrapi.obj" \
	"$(INTDIR)\jsxml.obj" \
	"$(INTDIR)\prmjtime.obj" \
	"$(OUTDIR)\fdlibm.lib"

"$(OUTDIR)\js32.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "js - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "js___Wi2"
# PROP BASE Intermediate_Dir "js___Wi2"
# PROP BASE Target_Dir ""
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Target_Dir ""
OUTDIR=.\Debug
INTDIR=.\Debug

ALL : "fdlibm - Win32 Debug" "jskwgen - Win32 Debug" "$(OUTDIR)\js32.dll"

CLEAN : 
	-@erase "$(INTDIR)\jsapi.obj"
	-@erase "$(INTDIR)\jsarena.obj"
	-@erase "$(INTDIR)\jsarray.obj"
	-@erase "$(INTDIR)\jsatom.obj"
	-@erase "$(INTDIR)\jsbool.obj"
	-@erase "$(INTDIR)\jscntxt.obj"
	-@erase "$(INTDIR)\jsdate.obj"
	-@erase "$(INTDIR)\jsdbgapi.obj"
	-@erase "$(INTDIR)\jsdhash.obj"
	-@erase "$(INTDIR)\jsdtoa.obj"
	-@erase "$(INTDIR)\jsemit.obj"
	-@erase "$(INTDIR)\jsexn.obj"
	-@erase "$(INTDIR)\jsfun.obj"
	-@erase "$(INTDIR)\jsgc.obj"
	-@erase "$(INTDIR)\jshash.obj"
	-@erase "$(INTDIR)\jsinterp.obj"
	-@erase "$(INTDIR)\jslock.obj"
	-@erase "$(INTDIR)\jslog2.obj"
	-@erase "$(INTDIR)\jslong.obj"
	-@erase "$(INTDIR)\jsmath.obj"
	-@erase "$(INTDIR)\jsnum.obj"
	-@erase "$(INTDIR)\jsobj.obj"
	-@erase "$(INTDIR)\jsopcode.obj"
	-@erase "$(INTDIR)\jsparse.obj"
	-@erase "$(INTDIR)\jsprf.obj"
	-@erase "$(INTDIR)\jsregexp.obj"
	-@erase "$(INTDIR)\jsscan.obj"
	-@erase "$(INTDIR)\jsscope.obj"
	-@erase "$(INTDIR)\jsscript.obj"
	-@erase "$(INTDIR)\jsstr.obj"
	-@erase "$(INTDIR)\jsutil.obj"
	-@erase "$(INTDIR)\jsxdrapi.obj"
	-@erase "$(INTDIR)\jsxml.obj"
	-@erase "$(INTDIR)\prmjtime.obj"
	-@erase "$(INTDIR)\js.pch"
	-@erase "$(INTDIR)\jsautokw.h"
	-@erase "$(OUTDIR)\js32.dll"
	-@erase "$(OUTDIR)\js32.exp"
	-@erase "$(OUTDIR)\js32.ilk"
	-@erase "$(OUTDIR)\js32.lib"
	-@erase "$(OUTDIR)\js32.pdb"
	-@$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="fdlibm - Win32 Debug" clean
	-@$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="jskwgen - Win32 Debug" clean

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /MTd /W3 /Gm /GX /Zi /Od /D "WIN32" /D "_DEBUG" /D _X86_=1 /D "_WINDOWS" /YX /c
# ADD CPP /nologo /MDd /W3 /Gm /GX /Zi /Od /D "_DEBUG" /D "DEBUG" /D _X86_=1 /D "_WINDOWS" /D "WIN32" /D "XP_WIN" /D "JSFILE" /D "EXPORT_JS_API" /I"$(INTDIR)" /YX /c
CPP_PROJ=/nologo /MDd /W3 /Gm /GX /Zi /Od /D "_DEBUG" /D "DEBUG" /D _X86_=1 /D "_WINDOWS"\
 /D "WIN32" /D "XP_WIN" /D "JSFILE" /D "EXPORT_JS_API" /Fp"$(INTDIR)/js.pch" /I"$(INTDIR)" /YX\
 /Fo"$(INTDIR)/" /Fd"$(INTDIR)/" /c 
CPP_OBJS=.\Debug/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

MTL=mktyplib.exe
# ADD BASE MTL /nologo /D "_DEBUG" /win32
# ADD MTL /nologo /D "_DEBUG" /win32
MTL_PROJ=/nologo /D "_DEBUG" /win32 
RSC=rc.exe
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/js.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:windows /dll /debug /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:windows /dll /debug /machine:I386 /out:"Debug/js32.dll"
# SUBTRACT LINK32 /nodefaultlib
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /nologo /subsystem:windows /dll /incremental:yes\
 /pdb:"$(OUTDIR)/js32.pdb" /debug /machine:I386 /out:"$(OUTDIR)/js32.dll"\
 /implib:"$(OUTDIR)/js32.lib" 
LINK32_OBJS= \
	"$(INTDIR)\jsapi.obj" \
	"$(INTDIR)\jsarena.obj" \
	"$(INTDIR)\jsarray.obj" \
	"$(INTDIR)\jsatom.obj" \
	"$(INTDIR)\jsbool.obj" \
	"$(INTDIR)\jscntxt.obj" \
	"$(INTDIR)\jsdate.obj" \
	"$(INTDIR)\jsdbgapi.obj" \
	"$(INTDIR)\jsdhash.obj" \
	"$(INTDIR)\jsdtoa.obj" \
	"$(INTDIR)\jsemit.obj" \
	"$(INTDIR)\jsexn.obj" \
	"$(INTDIR)\jsfun.obj" \
	"$(INTDIR)\jsgc.obj" \
	"$(INTDIR)\jshash.obj" \
	"$(INTDIR)\jsinterp.obj" \
	"$(INTDIR)\jslock.obj" \
	"$(INTDIR)\jslog2.obj" \
	"$(INTDIR)\jslong.obj" \
	"$(INTDIR)\jsmath.obj" \
	"$(INTDIR)\jsnum.obj" \
	"$(INTDIR)\jsobj.obj" \
	"$(INTDIR)\jsopcode.obj" \
	"$(INTDIR)\jsparse.obj" \
	"$(INTDIR)\jsprf.obj" \
	"$(INTDIR)\jsregexp.obj" \
	"$(INTDIR)\jsscan.obj" \
	"$(INTDIR)\jsscope.obj" \
	"$(INTDIR)\jsscript.obj" \
	"$(INTDIR)\jsstr.obj" \
	"$(INTDIR)\jsutil.obj" \
	"$(INTDIR)\jsxdrapi.obj" \
	"$(INTDIR)\jsxml.obj" \
	"$(INTDIR)\prmjtime.obj" \
	"$(OUTDIR)\fdlibm.lib"

"$(OUTDIR)\js32.dll" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "jskwgen - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "jsshell\Release"
# PROP BASE Intermediate_Dir "jskwgen\Release"
# PROP BASE Target_Dir "jskwgen"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Target_Dir "jskwgen"
OUTDIR=.\Release
INTDIR=.\Release

ALL : "$(INTDIR)" "$(INTDIR)\host_jskwgen.exe"

CLEAN : 
	-@erase "$(INTDIR)\jskwgen.obj"
	-@erase "$(INTDIR)\jskwgen.pch"
	-@erase "$(INTDIR)\host_jskwgen.exe"

"$(INTDIR)" :
    if not exist "$(INTDIR)/$(NULL)" mkdir "$(INTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /YX /c
# ADD CPP /nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "_CONSOLE" /D "WIN32" /D "XP_WIN" /D "JSFILE" /YX /c
CPP_PROJ=/nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "_CONSOLE" /D "WIN32" /D\
 "XP_WIN" /D "JSFILE" /Fp"$(INTDIR)/jskwgen.pch" /YX /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\Release/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

RSC=rc.exe
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(INTDIR)/jskwgen.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /nologo /subsystem:console /incremental:no\
 /pdb:"$(INTDIR)/jskwgen.pdb" /machine:I386 /out:"$(INTDIR)/host_jskwgen.exe" 
LINK32_OBJS= \
	"$(INTDIR)\jskwgen.obj" \

"$(INTDIR)\host_jskwgen.exe" : "$(INTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "jskwgen - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "jsshell\Debug"
# PROP BASE Intermediate_Dir "jskwgen\Debug"
# PROP BASE Target_Dir "jskwgen"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Target_Dir "jskwgen"
OUTDIR=.\Debug
INTDIR=.\Debug

ALL : "$(INTDIR)" "$(INTDIR)\host_jskwgen.exe"

CLEAN : 
	-@erase "$(INTDIR)\jskwgen.obj"
	-@erase "$(INTDIR)\jskwgen.pch"
	-@erase "$(INTDIR)\host_jskwgen.exe"

"$(INTDIR)" :
    if not exist "$(INTDIR)/$(NULL)" mkdir "$(INTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /YX /c
# ADD CPP /nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "_CONSOLE" /D "WIN32" /D "XP_WIN" /D "JSFILE" /YX /c
CPP_PROJ=/nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "_CONSOLE" /D "WIN32" /D\
 "XP_WIN" /D "JSFILE" /Fp"$(INTDIR)/jskwgen.pch" /YX /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\Debug/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

RSC=rc.exe
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(INTDIR)/jskwgen.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /nologo /subsystem:console /incremental:no\
 /pdb:"$(INTDIR)/jskwgen.pdb" /machine:I386 /out:"$(INTDIR)/host_jskwgen.exe" 
LINK32_OBJS= \
	"$(INTDIR)\jskwgen.obj" \

"$(INTDIR)\host_jskwgen.exe" : "$(INTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "jsshell - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "jsshell\Release"
# PROP BASE Intermediate_Dir "jsshell\Release"
# PROP BASE Target_Dir "jsshell"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Target_Dir "jsshell"
OUTDIR=.\Release
INTDIR=.\Release

ALL : "js - Win32 Release" "$(OUTDIR)\jsshell.exe"

CLEAN : 
	-@erase "$(INTDIR)\js.obj"
	-@erase "$(INTDIR)\jsshell.pch"
	-@erase "$(OUTDIR)\jsshell.exe"
	-@$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="js - Win32 Release" clean


"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D "_CONSOLE" /YX /c
# ADD CPP /nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "_CONSOLE" /D "WIN32" /D "XP_WIN" /D "JSFILE" /I"$(INTDIR)" /YX /c
CPP_PROJ=/nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "_CONSOLE" /D "WIN32" /D\
 "XP_WIN" /D "JSFILE" /Fp"$(INTDIR)/jsshell.pch" /I"$(INTDIR)" /YX /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\Release/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

RSC=rc.exe
# ADD BASE RSC /l 0x409 /d "NDEBUG"
# ADD RSC /l 0x409 /d "NDEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/jsshell.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /machine:I386
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /nologo /subsystem:console /incremental:no\
 /pdb:"$(OUTDIR)/jsshell.pdb" /machine:I386 /out:"$(OUTDIR)/jsshell.exe" 
LINK32_OBJS= \
	"$(INTDIR)\js.obj" \
	"$(OUTDIR)\js32.lib"

"$(OUTDIR)\jsshell.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "jsshell - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "jsshell\jsshell_"
# PROP BASE Intermediate_Dir "jsshell\jsshell_"
# PROP BASE Target_Dir "jsshell"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Target_Dir "jsshell"
OUTDIR=.\Debug
INTDIR=.\Debug

ALL : "js - Win32 Debug" "$(OUTDIR)\jsshell.exe"

CLEAN : 
	-@erase "$(INTDIR)\js.obj"
	-@erase "$(INTDIR)\jsshell.pch"
	-@erase "$(OUTDIR)\jsshell.exe"
	-@erase "$(OUTDIR)\jsshell.ilk"
	-@erase "$(OUTDIR)\jsshell.pdb"
	-@$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="js - Win32 Debug" clean

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /W3 /Gm /GX /Zi /Od /D "WIN32" /D "_DEBUG" /D "_CONSOLE" /YX /c
# ADD CPP /nologo /MDd /W3 /Gm /GX /Zi /Od /D "_CONSOLE" /D "_DEBUG" /D "WIN32" /D "XP_WIN" /D "JSFILE" /D "DEBUG" /YX /c
CPP_PROJ=/nologo /MDd /W3 /Gm /GX /Zi /Od /D "_CONSOLE" /D "_DEBUG" /D "WIN32"\
 /D "XP_WIN" /D "JSFILE" /D "DEBUG" /Fp"$(INTDIR)/jsshell.pch" /YX\
 /Fo"$(INTDIR)/" /Fd"$(INTDIR)/" /c 
CPP_OBJS=.\Debug/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

RSC=rc.exe
# ADD BASE RSC /l 0x409 /d "_DEBUG"
# ADD RSC /l 0x409 /d "_DEBUG"
BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/jsshell.bsc" 
BSC32_SBRS= \
	
LINK32=link.exe
# ADD BASE LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /debug /machine:I386
# ADD LINK32 kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib /nologo /subsystem:console /debug /machine:I386
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /nologo /subsystem:console /incremental:yes\
 /pdb:"$(OUTDIR)/jsshell.pdb" /debug /machine:I386 /out:"$(OUTDIR)/jsshell.exe" 
LINK32_OBJS= \
	"$(INTDIR)\js.obj" \
	"$(OUTDIR)\js32.lib"

"$(OUTDIR)\jsshell.exe" : "$(OUTDIR)" $(DEF_FILE) $(LINK32_OBJS)
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<

!ELSEIF  "$(CFG)" == "fdlibm - Win32 Release"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 0
# PROP BASE Output_Dir "fdlibm\Release"
# PROP BASE Intermediate_Dir "fdlibm\Release"
# PROP BASE Target_Dir "fdlibm"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 0
# PROP Output_Dir "Release"
# PROP Intermediate_Dir "Release"
# PROP Target_Dir "fdlibm"
OUTDIR=.\Release
INTDIR=.\Release

ALL : "$(OUTDIR)\fdlibm.lib"

CLEAN : 
	-@erase "$(INTDIR)\e_atan2.obj"
	-@erase "$(INTDIR)\e_pow.obj"
	-@erase "$(INTDIR)\e_sqrt.obj"
	-@erase "$(INTDIR)\k_standard.obj"
	-@erase "$(INTDIR)\s_atan.obj"
	-@erase "$(INTDIR)\s_copysign.obj"
	-@erase "$(INTDIR)\s_fabs.obj"
	-@erase "$(INTDIR)\s_finite.obj"
	-@erase "$(INTDIR)\s_isnan.obj"
	-@erase "$(INTDIR)\s_matherr.obj"
	-@erase "$(INTDIR)\s_rint.obj"
	-@erase "$(INTDIR)\s_scalbn.obj"
	-@erase "$(INTDIR)\w_atan2.obj"
	-@erase "$(INTDIR)\w_pow.obj"
	-@erase "$(INTDIR)\w_sqrt.obj"
	-@erase "$(INTDIR)\fdlibm.pch"
	-@erase "$(OUTDIR)\fdlibm.lib"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /W3 /GX /O2 /D "WIN32" /D "NDEBUG" /D _X86_=1 /D "_WINDOWS" /YX /c
# ADD CPP /nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "WIN32" /D _X86_=1 /D "_WINDOWS" /D "_IEEE_LIBM" /YX /c
CPP_PROJ=/nologo /MD /W3 /GX /O2 /D "NDEBUG" /D "WIN32" /D _X86_=1 /D "_WINDOWS" /D\
 "_IEEE_LIBM" /D "XP_WIN" /I .\ /Fp"$(INTDIR)/fdlibm.pch" /YX /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\Release/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/fdlibm.bsc" 
BSC32_SBRS= \
	
LIB32=link.exe -lib
# ADD BASE LIB32 /nologo
# ADD LIB32 /nologo
LIB32_FLAGS=/nologo /out:"$(OUTDIR)/fdlibm.lib" 
LIB32_OBJS= \
	"$(INTDIR)\e_atan2.obj" \
	"$(INTDIR)\e_pow.obj" \
	"$(INTDIR)\e_sqrt.obj" \
	"$(INTDIR)\k_standard.obj" \
	"$(INTDIR)\s_atan.obj" \
	"$(INTDIR)\s_copysign.obj" \
	"$(INTDIR)\s_fabs.obj" \
	"$(INTDIR)\s_finite.obj" \
	"$(INTDIR)\s_isnan.obj" \
	"$(INTDIR)\s_matherr.obj" \
	"$(INTDIR)\s_rint.obj" \
	"$(INTDIR)\s_scalbn.obj" \
	"$(INTDIR)\w_atan2.obj" \
	"$(INTDIR)\w_pow.obj" \
	"$(INTDIR)\w_sqrt.obj"

"$(OUTDIR)\fdlibm.lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
    $(LIB32) @<<
  $(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)
<<

!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

# PROP BASE Use_MFC 0
# PROP BASE Use_Debug_Libraries 1
# PROP BASE Output_Dir "fdlibm\Debug"
# PROP BASE Intermediate_Dir "fdlibm\Debug"
# PROP BASE Target_Dir "fdlibm"
# PROP Use_MFC 0
# PROP Use_Debug_Libraries 1
# PROP Output_Dir "Debug"
# PROP Intermediate_Dir "Debug"
# PROP Target_Dir "fdlibm"
OUTDIR=.\Debug
INTDIR=.\Debug

ALL : "$(OUTDIR)\fdlibm.lib"

CLEAN : 
	-@erase "$(INTDIR)\e_atan2.obj"
	-@erase "$(INTDIR)\e_pow.obj"
	-@erase "$(INTDIR)\e_sqrt.obj"
	-@erase "$(INTDIR)\k_standard.obj"
	-@erase "$(INTDIR)\s_atan.obj"
	-@erase "$(INTDIR)\s_copysign.obj"
	-@erase "$(INTDIR)\s_fabs.obj"
	-@erase "$(INTDIR)\s_finite.obj"
	-@erase "$(INTDIR)\s_isnan.obj"
	-@erase "$(INTDIR)\s_matherr.obj"
	-@erase "$(INTDIR)\s_rint.obj"
	-@erase "$(INTDIR)\s_scalbn.obj"
	-@erase "$(INTDIR)\w_atan2.obj"
	-@erase "$(INTDIR)\w_pow.obj"
	-@erase "$(INTDIR)\w_sqrt.obj"
	-@erase "$(INTDIR)\fdlibm.pch"
	-@erase "$(OUTDIR)\fdlibm.lib"

"$(OUTDIR)" :
    if not exist "$(OUTDIR)/$(NULL)" mkdir "$(OUTDIR)"

CPP=cl.exe
# ADD BASE CPP /nologo /W3 /GX /Z7 /Od /D "WIN32" /D "_DEBUG" /D _X86_=1 /D "_WINDOWS" /YX /c
# ADD CPP /nologo /MDd /W3 /GX /Z7 /Od /D "_DEBUG" /D "WIN32" /D _X86_=1 /D "_WINDOWS" /D "_IEEE_LIBM" /YX /c
CPP_PROJ=/nologo /MDd /W3 /GX /Z7 /Od /D "_DEBUG" /D "WIN32" /D _X86_=1 /D "_WINDOWS" /D\
 "_IEEE_LIBM" /D "XP_WIN" -I .\ /Fp"$(INTDIR)/fdlibm.pch" /YX /Fo"$(INTDIR)/" /c 
CPP_OBJS=.\Debug/
CPP_SBRS=.\.

.c{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_OBJS)}.obj:
   $(CPP) $(CPP_PROJ) $<  

.c{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cpp{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

.cxx{$(CPP_SBRS)}.sbr:
   $(CPP) $(CPP_PROJ) $<  

BSC32=bscmake.exe
# ADD BASE BSC32 /nologo
# ADD BSC32 /nologo
BSC32_FLAGS=/nologo /o"$(OUTDIR)/fdlibm.bsc" 
BSC32_SBRS= \
	
LIB32=link.exe -lib
# ADD BASE LIB32 /nologo
# ADD LIB32 /nologo
LIB32_FLAGS=/nologo /out:"$(OUTDIR)/fdlibm.lib" 
LIB32_OBJS= \
	"$(INTDIR)\e_atan2.obj" \
	"$(INTDIR)\e_pow.obj" \
	"$(INTDIR)\e_sqrt.obj" \
	"$(INTDIR)\k_standard.obj" \
	"$(INTDIR)\s_atan.obj" \
	"$(INTDIR)\s_copysign.obj" \
	"$(INTDIR)\s_fabs.obj" \
	"$(INTDIR)\s_finite.obj" \
	"$(INTDIR)\s_isnan.obj" \
	"$(INTDIR)\s_matherr.obj" \
	"$(INTDIR)\s_rint.obj" \
	"$(INTDIR)\s_scalbn.obj" \
	"$(INTDIR)\w_atan2.obj" \
	"$(INTDIR)\w_pow.obj" \
	"$(INTDIR)\w_sqrt.obj"

"$(OUTDIR)\fdlibm.lib" : "$(OUTDIR)" $(DEF_FILE) $(LIB32_OBJS)
    $(LIB32) @<<
  $(LIB32_FLAGS) $(DEF_FLAGS) $(LIB32_OBJS)
<<

!ENDIF 

################################################################################
# Begin Target

# Name "js - Win32 Release"
# Name "js - Win32 Debug"

!IF  "$(CFG)" == "js - Win32 Release"

!ELSEIF  "$(CFG)" == "js - Win32 Debug"

!ENDIF 

################################################################################
# Begin Source File

SOURCE=.\jsapi.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSAPI=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdate.h"\
	".\jsemit.h"\
	".\jsexn.h"\
	".\jsfile.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsmath.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSAPI=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsapi.obj" : $(SOURCE) $(DEP_CPP_JSAPI) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSAPI=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdate.h"\
	".\jsemit.h"\
	".\jsexn.h"\
	".\jsfile.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsmath.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSAPI=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsapi.obj" : $(SOURCE) $(DEP_CPP_JSAPI) "$(INTDIR)"

!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsarena.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSARE=\
	".\jsarena.h"\
	".\jsbit.h"\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsstddef.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSARE=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsarena.obj" : $(SOURCE) $(DEP_CPP_JSARE) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSARE=\
	".\jsarena.h"\
	".\jsbit.h"\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsstddef.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSARE=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsarena.obj" : $(SOURCE) $(DEP_CPP_JSARE) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsarray.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSARR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSARR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsarray.obj" : $(SOURCE) $(DEP_CPP_JSARR) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSARR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSARR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsarray.obj" : $(SOURCE) $(DEP_CPP_JSARR) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsatom.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSATO=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSATO=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsatom.obj" : $(SOURCE) $(DEP_CPP_JSATO) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSATO=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSATO=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsatom.obj" : $(SOURCE) $(DEP_CPP_JSATO) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsbool.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSBOO=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSBOO=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsbool.obj" : $(SOURCE) $(DEP_CPP_JSBOO) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSBOO=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSBOO=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsbool.obj" : $(SOURCE) $(DEP_CPP_JSBOO) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jscntxt.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSCNT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsexn.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSCNT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jscntxt.obj" : $(SOURCE) $(DEP_CPP_JSCNT) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSCNT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsexn.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSCNT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jscntxt.obj" : $(SOURCE) $(DEP_CPP_JSCNT) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsdate.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSDAT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdate.h"\
	".\jsdtoa.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\prmjtime.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDAT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsdate.obj" : $(SOURCE) $(DEP_CPP_JSDAT) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSDAT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdate.h"\
	".\jsdtoa.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\prmjtime.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDAT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsdate.obj" : $(SOURCE) $(DEP_CPP_JSDAT) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsdbgapi.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSDBG=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDBG=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsdbgapi.obj" : $(SOURCE) $(DEP_CPP_JSDBG) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSDBG=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDBG=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsdbgapi.obj" : $(SOURCE) $(DEP_CPP_JSDBG) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsdhash.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSDHA=\
	".\jsbit.h"\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jsdhash.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDHA=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsdhash.obj" : $(SOURCE) $(DEP_CPP_JSDHA) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSDHA=\
	".\jsbit.h"\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jsdhash.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDHA=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsdhash.obj" : $(SOURCE) $(DEP_CPP_JSDHA) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsdtoa.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSDTO=\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jsdtoa.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsstddef.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDTO=\
	".\jsautocfg.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsdtoa.obj" : $(SOURCE) $(DEP_CPP_JSDTO) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSDTO=\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jsdtoa.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsstddef.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSDTO=\
	".\jsautocfg.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsdtoa.obj" : $(SOURCE) $(DEP_CPP_JSDTO) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsemit.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSEMI=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSEMI=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsemit.obj" : $(SOURCE) $(DEP_CPP_JSEMI) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSEMI=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSEMI=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsemit.obj" : $(SOURCE) $(DEP_CPP_JSEMI) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsexn.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSEXN=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsexn.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSEXN=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsexn.obj" : $(SOURCE) $(DEP_CPP_JSEXN) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSEXN=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsexn.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSEXN=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsexn.obj" : $(SOURCE) $(DEP_CPP_JSEXN) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsfun.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSFUN=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSFUN=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsfun.obj" : $(SOURCE) $(DEP_CPP_JSFUN) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSFUN=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSFUN=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsfun.obj" : $(SOURCE) $(DEP_CPP_JSFUN) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsgc.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSGC_=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSGC_=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsgc.obj" : $(SOURCE) $(DEP_CPP_JSGC_) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSGC_=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSGC_=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsgc.obj" : $(SOURCE) $(DEP_CPP_JSGC_) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jshash.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSHAS=\
	".\jsbit.h"\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jshash.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSHAS=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jshash.obj" : $(SOURCE) $(DEP_CPP_JSHAS) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSHAS=\
	".\jsbit.h"\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jshash.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSHAS=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jshash.obj" : $(SOURCE) $(DEP_CPP_JSHAS) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsinterp.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSINT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSINT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsinterp.obj" : $(SOURCE) $(DEP_CPP_JSINT) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSINT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSINT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsinterp.obj" : $(SOURCE) $(DEP_CPP_JSINT) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jslock.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSLOC=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSLOC=\
	".\jsautocfg.h"\
	".\pratom.h"\
	".\prcvar.h"\
	".\prlock.h"\
	".\prthread.h"\
	

"$(INTDIR)\jslock.obj" : $(SOURCE) $(DEP_CPP_JSLOC) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSLOC=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSLOC=\
	".\jsautocfg.h"\
	".\pratom.h"\
	".\prcvar.h"\
	".\prlock.h"\
	".\prthread.h"\
	

"$(INTDIR)\jslock.obj" : $(SOURCE) $(DEP_CPP_JSLOC) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jslog2.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSLOG=\
	".\jsbit.h"\
	".\jscpucfg.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSLOG=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jslog2.obj" : $(SOURCE) $(DEP_CPP_JSLOG) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSLOG=\
	".\jsbit.h"\
	".\jscpucfg.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSLOG=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jslog2.obj" : $(SOURCE) $(DEP_CPP_JSLOG) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jslong.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSLON=\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSLON=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jslong.obj" : $(SOURCE) $(DEP_CPP_JSLON) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSLON=\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jstypes.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSLON=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jslong.obj" : $(SOURCE) $(DEP_CPP_JSLON) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsmath.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSMAT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslibmath.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsmath.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\prmjtime.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSMAT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsmath.obj" : $(SOURCE) $(DEP_CPP_JSMAT) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSMAT=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslibmath.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsmath.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\prmjtime.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSMAT=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsmath.obj" : $(SOURCE) $(DEP_CPP_JSMAT) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsnum.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSNUM=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdtoa.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSNUM=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsnum.obj" : $(SOURCE) $(DEP_CPP_JSNUM) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSNUM=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdtoa.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSNUM=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsnum.obj" : $(SOURCE) $(DEP_CPP_JSNUM) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsobj.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSOBJ=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSOBJ=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsobj.obj" : $(SOURCE) $(DEP_CPP_JSOBJ) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSOBJ=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSOBJ=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsobj.obj" : $(SOURCE) $(DEP_CPP_JSOBJ) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsopcode.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSOPC=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsdtoa.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSOPC=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsopcode.obj" : $(SOURCE) $(DEP_CPP_JSOPC) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSOPC=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsdtoa.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSOPC=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsopcode.obj" : $(SOURCE) $(DEP_CPP_JSOPC) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsparse.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSPAR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSPAR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsparse.obj" : $(SOURCE) $(DEP_CPP_JSPAR) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSPAR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSPAR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsparse.obj" : $(SOURCE) $(DEP_CPP_JSPAR) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsprf.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSPRF=\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSPRF=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsprf.obj" : $(SOURCE) $(DEP_CPP_JSPRF) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSPRF=\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSPRF=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsprf.obj" : $(SOURCE) $(DEP_CPP_JSPRF) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsregexp.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSREG=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSREG=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsregexp.obj" : $(SOURCE) $(DEP_CPP_JSREG) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSREG=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSREG=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsregexp.obj" : $(SOURCE) $(DEP_CPP_JSREG) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsscan.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSSCA=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdtoa.h"\
	".\jsexn.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSSCA=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsscan.obj" : $(SOURCE) $(DEP_CPP_JSSCA) "$(INTDIR)" "$(INTDIR)\jsautokw.h"

"$(INTDIR)\jsautokw.h" : $(INTDIR)\host_jskwgen.exe jskeyword.tbl
	$(INTDIR)\host_jskwgen.exe $(INTDIR)\jsautokw.h

!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSSCA=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdtoa.h"\
	".\jsexn.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	$(INTDIR)\jsautokw.h \
	
NODEP_CPP_JSSCA=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsscan.obj" : $(SOURCE) $(DEP_CPP_JSSCA) "$(INTDIR)"

"$(INTDIR)\jsautokw.h" : $(INTDIR)\host_jskwgen.exe jskeyword.tbl
	$(INTDIR)\host_jskwgen.exe $(INTDIR)\jsautokw.h

!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jskwgen.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSSCO=\
	".\jskwgen.c"\
	{$(INCLUDE)}"\sys\types.h"\
		

"$(INTDIR)\jskwgen.obj" : $(SOURCE) $(DEP_CPP_JSSCO) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSSCO=\
	".\jskwgen.c"\
	{$(INCLUDE)}"\sys\types.h"\
		
LINK32_FLAGS=kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib\
 advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib odbc32.lib\
 odbccp32.lib /nologo /subsystem:console /incremental:no\
 /pdb:"$(INTDIR)/host_jskwgen.pdb" /machine:I386 /out:"$(INTDIR)/host_jskwgen.exe" 

LINK32_OBJS= \
	"$(INTDIR)\jskwgen.obj"

"$(INTDIR)\host_jskwgen.exe" : "$(INTDIR)" $(SOURCE) $(DEP_CPP_JSSCO) "$(INTDIR)"
    $(LINK32) @<<
  $(LINK32_FLAGS) $(LINK32_OBJS)
<<


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsscope.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSSCO=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSSCO=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsscope.obj" : $(SOURCE) $(DEP_CPP_JSSCO) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSSCO=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSSCO=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsscope.obj" : $(SOURCE) $(DEP_CPP_JSSCO) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsscript.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSSCR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSSCR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsscript.obj" : $(SOURCE) $(DEP_CPP_JSSCR) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSSCR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSSCR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsscript.obj" : $(SOURCE) $(DEP_CPP_JSSCR) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsstr.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSSTR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSSTR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsstr.obj" : $(SOURCE) $(DEP_CPP_JSSTR) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSSTR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbool.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSSTR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsstr.obj" : $(SOURCE) $(DEP_CPP_JSSTR) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsutil.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSUTI=\
	".\jscpucfg.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSUTI=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsutil.obj" : $(SOURCE) $(DEP_CPP_JSUTI) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSUTI=\
	".\jscpucfg.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSUTI=\
	".\jsautocfg.h"\
	

"$(INTDIR)\jsutil.obj" : $(SOURCE) $(DEP_CPP_JSUTI) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsxdrapi.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSXDR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSXDR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsxdrapi.obj" : $(SOURCE) $(DEP_CPP_JSXDR) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSXDR=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscope.h"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxdrapi.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSXDR=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsxdrapi.obj" : $(SOURCE) $(DEP_CPP_JSXDR) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\jsxml.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_JSXML=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbit.h"\
	".\jsbool.h"\
	".\jscntxt.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsparse.h"\
	".\jsprf.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSXML=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsxml.obj" : $(SOURCE) $(DEP_CPP_JSXML) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_JSXML=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarray.h"\
	".\jsatom.h"\
	".\jsbit.h"\
	".\jsbool.h"\
	".\jscntxt.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jsnum.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsparse.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	".\jsxml.h"\
	".\jsprf.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JSXML=\
	".\jsautocfg.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\jsxml.obj" : $(SOURCE) $(DEP_CPP_JSXML) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\prmjtime.c

!IF  "$(CFG)" == "js - Win32 Release"

DEP_CPP_PRMJT=\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jstypes.h"\
	".\prmjtime.h"\
	{$(INCLUDE)}"\sys\TIMEB.H"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_PRMJT=\
	".\jsautocfg.h"\
	

"$(INTDIR)\prmjtime.obj" : $(SOURCE) $(DEP_CPP_PRMJT) "$(INTDIR)"


!ELSEIF  "$(CFG)" == "js - Win32 Debug"

DEP_CPP_PRMJT=\
	".\jscompat.h"\
	".\jscpucfg.h"\
	".\jslong.h"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsprf.h"\
	".\jstypes.h"\
	".\prmjtime.h"\
	{$(INCLUDE)}"\sys\TIMEB.H"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_PRMJT=\
	".\jsautocfg.h"\
	

"$(INTDIR)\prmjtime.obj" : $(SOURCE) $(DEP_CPP_PRMJT) "$(INTDIR)"


!ENDIF 

# End Source File
################################################################################
# Begin Project Dependency

# Project_Dep_Name "fdlibm"

!IF  "$(CFG)" == "js - Win32 Debug"

"fdlibm - Win32 Debug" : 
   @$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="fdlibm - Win32 Debug" 

!ELSEIF  "$(CFG)" == "js - Win32 Release"

"fdlibm - Win32 Release" : 
   @$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="fdlibm - Win32 Release" 

!ENDIF 

# End Project Dependency
# End Target
################################################################################
# Begin Target

# Name "jsshell - Win32 Release"
# Name "jsshell - Win32 Debug"

!IF  "$(CFG)" == "jsshell - Win32 Release"

!ELSEIF  "$(CFG)" == "jsshell - Win32 Debug"

!ENDIF 

################################################################################
# Begin Source File

SOURCE=.\js.c
DEP_CPP_JS_C42=\
	".\js.msg"\
	".\jsapi.h"\
	".\jsarena.h"\
	".\jsatom.h"\
	".\jsclist.h"\
	".\jscntxt.h"\
	".\jscompat.h"\
	".\jsconfig.h"\
	".\jscpucfg.h"\
	".\jsdbgapi.h"\
	".\jsemit.h"\
	".\jsfun.h"\
	".\jsgc.h"\
	".\jshash.h"\
	".\jsinterp.h"\
	".\jslock.h"\
	".\jslong.h"\
	".\jsobj.h"\
	".\jsopcode.h"\
	".\jsopcode.tbl"\
	".\jsosdep.h"\
	".\jsotypes.h"\
	".\jsparse.h"\
	".\jsprf.h"\
	".\jsprvtd.h"\
	".\jspubtd.h"\
	".\jsregexp.h"\
	".\jsscan.h"\
	".\jsscope.h"\
	".\jsscript.h"\
	".\jsshell.msg"\
	".\jsstddef.h"\
	".\jsstr.h"\
	".\jstypes.h"\
	".\jsutil.h"\
	{$(INCLUDE)}"\sys\types.h"\
	
NODEP_CPP_JS_C42=\
	".\jsautocfg.h"\
	".\jsdb.h"\
	".\jsdebug.h"\
	".\jsdjava.h"\
	".\jsjava.h"\
	".\jsperl.h"\
	".\prcvar.h"\
	".\prlock.h"\
	

"$(INTDIR)\js.obj" : $(SOURCE) $(DEP_CPP_JS_C42) "$(INTDIR)"


# End Source File
################################################################################
# Begin Project Dependency

# Project_Dep_Name "jskwgen"

!IF  "$(CFG)" == "js - Win32 Release"

"jskwgen - Win32 Release" : 
   @$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="jskwgen - Win32 Release" 

!ELSEIF  "$(CFG)" == "js - Win32 Debug"

"jskwgen - Win32 Debug" : 
   @$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="jskwgen - Win32 Debug" 

!ENDIF 

# End Project Dependency
# End Target
################################################################################
# Begin Project Dependency

# Project_Dep_Name "js"

!IF  "$(CFG)" == "jsshell - Win32 Release"

"js - Win32 Release" : 
   @$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="js - Win32 Release" 

!ELSEIF  "$(CFG)" == "jsshell - Win32 Debug"

"js - Win32 Debug" : 
   @$(MAKE) /nologo /$(MAKEFLAGS) /F ".\js.mak" CFG="js - Win32 Debug" 

!ENDIF 

# End Project Dependency
# End Target
################################################################################
# Begin Target

# Name "fdlibm - Win32 Release"
# Name "fdlibm - Win32 Debug"

!IF  "$(CFG)" == "fdlibm - Win32 Release"

!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

!ENDIF 

################################################################################
# Begin Source File

SOURCE=.\fdlibm\w_atan2.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_W_ATA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\w_atan2.obj" : $(SOURCE) $(DEP_CPP_W_ATA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_W_ATA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\w_atan2.obj" : $(SOURCE) $(DEP_CPP_W_ATA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_copysign.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_COP=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_copysign.obj" : $(SOURCE) $(DEP_CPP_S_COP) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_COP=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_copysign.obj" : $(SOURCE) $(DEP_CPP_S_COP) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\w_pow.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_W_POW=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\w_pow.obj" : $(SOURCE) $(DEP_CPP_W_POW) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_W_POW=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\w_pow.obj" : $(SOURCE) $(DEP_CPP_W_POW) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\e_pow.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_E_POW=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\e_pow.obj" : $(SOURCE) $(DEP_CPP_E_POW) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_E_POW=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\e_pow.obj" : $(SOURCE) $(DEP_CPP_E_POW) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\k_standard.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_K_STA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\k_standard.obj" : $(SOURCE) $(DEP_CPP_K_STA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_K_STA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\k_standard.obj" : $(SOURCE) $(DEP_CPP_K_STA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\e_atan2.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_E_ATA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\e_atan2.obj" : $(SOURCE) $(DEP_CPP_E_ATA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_E_ATA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\e_atan2.obj" : $(SOURCE) $(DEP_CPP_E_ATA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_isnan.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_ISN=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_isnan.obj" : $(SOURCE) $(DEP_CPP_S_ISN) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_ISN=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_isnan.obj" : $(SOURCE) $(DEP_CPP_S_ISN) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_fabs.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_FAB=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_fabs.obj" : $(SOURCE) $(DEP_CPP_S_FAB) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_FAB=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_fabs.obj" : $(SOURCE) $(DEP_CPP_S_FAB) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\w_sqrt.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_W_SQR=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\w_sqrt.obj" : $(SOURCE) $(DEP_CPP_W_SQR) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_W_SQR=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\w_sqrt.obj" : $(SOURCE) $(DEP_CPP_W_SQR) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_scalbn.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_SCA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_scalbn.obj" : $(SOURCE) $(DEP_CPP_S_SCA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_SCA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_scalbn.obj" : $(SOURCE) $(DEP_CPP_S_SCA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\e_sqrt.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_E_SQR=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\e_sqrt.obj" : $(SOURCE) $(DEP_CPP_E_SQR) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_E_SQR=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\e_sqrt.obj" : $(SOURCE) $(DEP_CPP_E_SQR) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_rint.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_RIN=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_rint.obj" : $(SOURCE) $(DEP_CPP_S_RIN) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_RIN=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_rint.obj" : $(SOURCE) $(DEP_CPP_S_RIN) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_atan.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_ATA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_atan.obj" : $(SOURCE) $(DEP_CPP_S_ATA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_ATA=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_atan.obj" : $(SOURCE) $(DEP_CPP_S_ATA) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_finite.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_FIN=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_finite.obj" : $(SOURCE) $(DEP_CPP_S_FIN) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_FIN=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_finite.obj" : $(SOURCE) $(DEP_CPP_S_FIN) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
################################################################################
# Begin Source File

SOURCE=.\fdlibm\s_matherr.c

!IF  "$(CFG)" == "fdlibm - Win32 Release"

DEP_CPP_S_MAT=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_matherr.obj" : $(SOURCE) $(DEP_CPP_S_MAT) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ELSEIF  "$(CFG)" == "fdlibm - Win32 Debug"

DEP_CPP_S_MAT=\
	".\fdlibm\fdlibm.h"\
	

"$(INTDIR)\s_matherr.obj" : $(SOURCE) $(DEP_CPP_S_MAT) "$(INTDIR)"
   $(CPP) $(CPP_PROJ) $(SOURCE)


!ENDIF 

# End Source File
# End Target
# End Project
################################################################################
