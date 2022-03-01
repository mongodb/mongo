# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
#**********************************************************************
#* Copyright (C) 1999-2008, International Business Machines Corporation
#* and others.  All Rights Reserved.
#**********************************************************************
# nmake file for creating data files on win32
# invoke with
# nmake /f makedata.mak icup=<path_to_icu_instalation> [Debug|Release]
#
#   12/10/1999  weiv    Created

#If no config, we default to debug
!IF "$(CFG)" == ""
CFG=Debug
!MESSAGE No configuration specified. Defaulting to common - Win32 Debug.
!ENDIF

#Here we test if a valid configuration is given
!IF "$(CFG)" != "Release" && "$(CFG)" != "release" && "$(CFG)" != "Debug" && "$(CFG)" != "debug" && "$(CFG)" != "x86\Release" && "$(CFG)" != "x86\Debug" && "$(CFG)" != "x64\Release" && "$(CFG)" != "x64\Debug" && "$(CFG)" != "ARM\Release" && "$(CFG)" != "ARM\Debug" && "$(CFG)" != "ARM64\Release" && "$(CFG)" != "ARM64\Debug"
!MESSAGE Invalid configuration "$(CFG)" specified.
!MESSAGE You can specify a configuration when running NMAKE
!MESSAGE by defining the macro CFG on the command line. For example:
!MESSAGE
!MESSAGE NMAKE /f "makedata.mak" CFG="Debug"
!MESSAGE
!MESSAGE Possible choices for configuration are:
!MESSAGE
!MESSAGE "Release"
!MESSAGE "Debug"
!MESSAGE
!ERROR An invalid configuration is specified.
!ENDIF

#Let's see if user has given us a path to ICU
#This could be found according to the path to makefile, but for now it is this way
!IF "$(ICUP)"==""
!ERROR Can't find path!
!ENDIF
!MESSAGE ICU path is $(ICUP)

RESNAME=uconvmsg
RESDIR=resources
RESFILES=resfiles.mk
ICUDATA=$(ICUP)\data

DLL_OUTPUT=.\$(CFG)
# set the following to 'static' or 'dll' depending
PKGMODE=static

ICD=$(ICUDATA)^\
DATA_PATH=$(ICUP)\data^\

# Use the x64 tools for building ARM and ARM64.
# Note: This is similar to the TOOLS CFG PATH in source\data\makedata.mak
!IF "$(CFG)" == "x64\Release" || "$(CFG)" == "x64\Debug" || "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM\Debug" || "$(CFG)" == "ARM64\Release"  || "$(CFG)" == "ARM64\Debug"
ICUTOOLS=$(ICUP)\bin64
PATH = $(ICUP)\bin64;$(PATH)
!ELSE
ICUTOOLS=$(ICUP)\bin
PATH = $(ICUP)\bin;$(PATH)
!ENDIF

# If building ARM/ARM, then we need to pass the arch as an argument.
EXTRA_PKGDATA_ARGUMENTS=
!IF "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM\Debug"
EXTRA_PKGDATA_ARGUMENTS=-a ARM
!ENDIF
!IF "$(CFG)" == "ARM64\Release" || "$(CFG)" == "ARM64\Debug"
EXTRA_PKGDATA_ARGUMENTS=-a ARM64
!ENDIF

# Make sure the necessary tools exist before continuing. (This is to prevent cryptic errors from NMAKE).
!IF !EXISTS($(ICUTOOLS)\pkgdata.exe)
!MESSAGE Unable to find "$(ICUTOOLS)\pkgdata.exe"
!ERROR The tool 'pkgdata.exe' does not exist! (Have you built all of ICU yet?).
!IF "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM\Debug" || "$(CFG)" == "ARM64\Release" || "$(CFG)" == "ARM64\Debug"
!ERROR Note that the ARM and ARM64 builds require building x64 first.
!ENDIF
!ENDIF
!IF !EXISTS($(ICUTOOLS)\genrb.exe)
!MESSAGE Unable to find "$(ICUTOOLS)\genrb.exe"
!ERROR The tool 'genrb.exe' does not exist! (Have you built all of ICU yet?).
!IF "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM\Debug" || "$(CFG)" == "ARM64\Release" || "$(CFG)" == "ARM64\Debug"
!ERROR Note that the ARM and ARM64 builds require building x64 first.
!ENDIF
!ENDIF

# Suffixes for data files
.SUFFIXES : .ucm .cnv .dll .dat .res .txt .c

# We're including a list of resource files.
FILESEPCHAR=

!IF EXISTS("$(RESFILES)")
!INCLUDE "$(RESFILES)"
!ELSE
!ERROR ERROR: cannot find "$(RESFILES)"
!ENDIF
RES_FILES = $(RESSRC:.txt=.res)
RB_FILES = resources\$(RES_FILES:.res =.res resources\)
RESOURCESDIR=

# This target should build all the data files
!IF "$(PKGMODE)" == "dll"
OUTPUT = "$(DLL_OUTPUT)\$(RESNAME).dll"
!ELSE
OUTPUT = "$(DLL_OUTPUT)\$(RESNAME).lib"
!ENDIF

ALL : $(OUTPUT)
    @echo All targets are up to date (mode $(PKGMODE))


# invoke pkgdata - static
"$(DLL_OUTPUT)\$(RESNAME).lib" : $(RB_FILES) $(RESFILES)
    @echo Building $(RESNAME).lib
    @"$(ICUTOOLS)\pkgdata" -f -v -m static -c -p $(RESNAME) -d "$(DLL_OUTPUT)" $(EXTRA_PKGDATA_ARGUMENTS) -s "$(RESDIR)" <<pkgdatain.txt
$(RES_FILES:.res =.res
)
<<KEEP

# This is to remove all the data files
CLEAN :
    -@erase "$(RB_FILES)"
    -@erase "$(CFG)\*uconvmsg*.*"
    -@"$(ICUTOOLS)\pkgdata" -f --clean -v -m static -c -p $(RESNAME) -d "$(DLL_OUTPUT)" $(EXTRA_PKGDATA_ARGUMENTS) -s "$(RESDIR)" pkgdatain.txt

# Inference rule for creating resource bundles
{$(RESDIR)}.txt{$(RESDIR)}.res:
    @echo Making Resource Bundle files
    "$(ICUTOOLS)\genrb" -s $(@D) -d $(@D) $(?F)

$(RESSRC) : {"$(ICUTOOLS)"}genrb.exe
