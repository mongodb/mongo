# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
#**********************************************************************
#* Copyright (C) 1999-2016, International Business Machines Corporation
#* and others.  All Rights Reserved.
#**********************************************************************
# nmake file for creating data files on win32
# invoke with
# nmake /f makedata.mak icumake=$(ProjectDir)
#
#	12/10/1999	weiv	Created

##############################################################################
# Keep the following in sync with the version - see common/unicode/uvernum.h
U_ICUDATA_NAME=icudt69
##############################################################################
!IF "$(UWP)" == "UWP"
# Optionally change the name of the data file for the UWP version.
U_ICUDATA_NAME=icudt69
!ENDIF
U_ICUDATA_ENDIAN_SUFFIX=l
UNICODE_VERSION=13.0
ICU_LIB_TARGET=$(DLL_OUTPUT)\$(U_ICUDATA_NAME).dll

#  ICUMAKE
#     Must be provided by whoever runs this makefile.
#     Is the directory containing this file (makedata.mak)
#     Is the directory into which most data is built (prior to packaging)
#     Is icu\source\data\
#
!IF "$(ICUMAKE)"==""
!ERROR Can't find ICUMAKE (ICU Data Make dir, should point to icu\source\data\ )!
!ENDIF
!MESSAGE ICU data make path is $(ICUMAKE)

!IF [py -3 -c "exit(0)"]!=0
!MESSAGE Information: Unable to find Python 3. Data will fail to build from source.
!ENDIF

# Suffixes for data files
.SUFFIXES : .nrm .icu .ucm .cnv .dll .dat .res .txt .c

ICUOUT=$(ICUMAKE)\out

#  the prefix "icudt62_" for use in filenames
ICUPKG=$(U_ICUDATA_NAME)$(U_ICUDATA_ENDIAN_SUFFIX)

# need to nuke \\ for .NET...
ICUOUT=$(ICUOUT:\\=\)

ICUBLD=$(ICUOUT)\build
ICUBLD_PKG=$(ICUBLD)\$(ICUPKG)
ICUTMP=$(ICUOUT)\tmp

#  ICUP
#     The root of the ICU source directory tree
#
ICUP=$(ICUMAKE)\..\..
ICUP=$(ICUP:\source\data\..\..=)
# In case the first one didn't do it, try this one.  .NET would do the second one.
ICUP=$(ICUP:\source\data\\..\..=)
!MESSAGE ICU root path is $(ICUP)


#  ICUSRCDATA
#       The data directory in source
#
ICUSRCDATA=$(ICUP)\source\data
ICUSRCDATA_RELATIVE_PATH=..\..\..

# Timestamp files to keep track of current build state
TOOLS_TS=$(ICUTMP)\tools.timestamp
COREDATA_TS=$(ICUTMP)\coredata.timestamp
ARM_CROSSBUILD_TS=

#  ICUCOL
#       The directory that contains colfiles.mk files along with *.txt collation data files
#
ICUCOL=coll

#  ICURBNF
#       The directory that contains rbnffiles.mk files along with *.txt RBNF data files
#
ICURBNF=rbnf

#  ICUTRNS
#       The directory that contains trfiles.mk files along with *.txt transliterator files
#
ICUTRNS=translit

#  ICUBRK
#       The directory that contains resfiles.mk files along with *.txt break iterator files
#
ICUBRK=brkitr

#
#  ICUDATA
#     The source directory.  Contains the source files for the common data to be built.
#     WARNING:  NOT THE SAME AS ICU_DATA environment variable.  Confusing.
ICUDATA=$(ICUP)\source\data

#
#  DLL_OUTPUT
#      Destination directory for the common data DLL file.
#      This is the same place that all of the other ICU DLLs go (the code-containing DLLs)
#      The lib file for the data DLL goes in $(DLL_OUTPUT)/../lib/
#
!IF "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM\Debug"
DLL_OUTPUT=$(ICUP)\binARM$(UWP)
!ELSE IF "$(CFG)" == "ARM64\Release" || "$(CFG)" == "ARM64\Debug"
DLL_OUTPUT=$(ICUP)\binARM64$(UWP)
!ELSE IF "$(CFG)" == "x64\Release" || "$(CFG)" == "x64\Debug"
DLL_OUTPUT=$(ICUP)\bin64$(UWP)
!ELSE IF "$(UWP)" == "UWP"
DLL_OUTPUT=$(ICUP)\bin32$(UWP)
!ELSE
DLL_OUTPUT=$(ICUP)\bin$(UWP)
!ENDIF
!MESSAGE ICU data DLL_OUTPUT path is $(DLL_OUTPUT)

#
#  TESTDATA
#     The source directory for data needed for test programs.
TESTDATA=$(ICUP)\source\test\testdata

#
#   TESTDATAOUT
#      The destination directory for the built test data .dat file
TESTDATAOUT=$(ICUP)\source\test\testdata\out

#
#   TESTDATABLD
#		The build directory for test data intermediate files
#		(Tests are NOT run from this makefile,
#         only the data is put in place.)
TESTDATABLD=$(ICUP)\source\test\testdata\out\build

#
#   ICUTOOLS
#       Directory under which all of the ICU data building tools live.
#
ICUTOOLS=$(ICUP)\source\tools
!MESSAGE ICU tools path is $(ICUTOOLS)

#   ARM_CROSS_BUILD
#       In order to support cross-compiling for ARM/ARM64 using the x64 tools
#       we need to know if we're building the ARM/ARM64 data DLL, otherwise
#       the existence of the x64 bits will cause us to think we are already done.
#    Note: This is only for the "regular" builds, the UWP builds have a separate project file entirely.
ARM_CROSS_BUILD=
!IF "$(UWP)" == ""
!IF "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM\Debug"
ARM_CROSS_BUILD=ARM
ARM_CROSSBUILD_TS=$(ICUTMP)\$(ARM_CROSS_BUILD).timestamp
!ELSE IF "$(CFG)" == "ARM64\Release"  || "$(CFG)" == "ARM64\Debug"
ARM_CROSS_BUILD=ARM64
ARM_CROSSBUILD_TS=$(ICUTMP)\$(ARM_CROSS_BUILD).timestamp
!ENDIF
!ENDIF

#
#  TOOLS CFG PATH
#      Generally the tools want to run on the same architecture as is being built.
#      Thus ARM and ARM64 need to use another build of the other tools, so make sure to get an usable CFG path.
#      Since tools, particularly pkggen, have architecture built-in, we made x64 on
#      Windows be machine-independent and use those tools for both ARM and ARM64.
#      Note: If we're building ARM/ARM64 Debug, then we'll use the x64 Debug tools.
#      If we're building ARM/ARM64 Release, then we'll use the x64 Release tools.
#
!IF "$(ARM_CROSS_BUILD)" == ""
CFGTOOLS=$(CFG)
!ELSE
!IF "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM64\Release"
CFGTOOLS=x64\Release
!ENDIF
!IF "$(CFG)" == "ARM\Debug" || "$(CFG)" == "ARM64\Debug"
CFGTOOLS=x64\Debug
!ENDIF
!ENDIF
!MESSAGE ICU tools CFG subpath is $(CFGTOOLS)


# The current ICU tools need to be in the path first.
# x86 uses x86; x64, arm, and arm64 use x64
!IF "$(CFG)" == "x86\Release" || "$(CFG)" == "x86\Debug"
PATH = $(ICUP)\bin;$(PATH)
ICUPBIN=$(ICUP)\bin
!ELSE
PATH = $(ICUP)\bin64;$(PATH)
ICUPBIN=$(ICUP)\bin64
!ENDIF


# This variable can be overridden to "-m static" by the project settings,
# if you want a static data library.
!IF "$(ICU_PACKAGE_MODE)"==""
ICU_PACKAGE_MODE=-m dll
!ENDIF

# If this archive exists, build from that
# instead of building everything from scratch.
ICUDATA_SOURCE_ARCHIVE=$(ICUSRCDATA)\in\$(ICUPKG).dat
!IF !EXISTS("$(ICUDATA_SOURCE_ARCHIVE)")
# Does a big endian version exist either?
ICUDATA_ARCHIVE=$(ICUSRCDATA)\in\$(U_ICUDATA_NAME)b.dat
!IF EXISTS("$(ICUDATA_ARCHIVE)")
ICUDATA_SOURCE_ARCHIVE=$(ICUTMP)\$(ICUPKG).dat
!ELSE
# Nothing was usable for input
!UNDEF ICUDATA_SOURCE_ARCHIVE
!ENDIF
!ENDIF

!IFDEF ICUDATA_SOURCE_ARCHIVE
!MESSAGE ICU data source archive is $(ICUDATA_SOURCE_ARCHIVE)
!ENDIF

# Common defines for both ways of building ICU's data library.
COMMON_ICUDATA_DEPENDENCIES="$(ICUPBIN)\pkgdata.exe" "$(ICUTMP)\icudata.res" "$(ICUP)\source\stubdata\stubdatabuilt.txt"
COMMON_ICUDATA_ARGUMENTS=-f -e $(U_ICUDATA_NAME) -v $(ICU_PACKAGE_MODE) -c -p $(ICUPKG) -T "$(ICUTMP)" -L $(U_ICUDATA_NAME) -d "$(ICUBLD_PKG)" -s .
!IF "$(UWP)" == "UWP"
COMMON_ICUDATA_ARGUMENTS=$(COMMON_ICUDATA_ARGUMENTS) -u
!ENDIF
!IF "$(CFG)" == "x64\Release" || "$(CFG)" == "x64\Debug"
COMMON_ICUDATA_ARGUMENTS=$(COMMON_ICUDATA_ARGUMENTS) -a X64
!ENDIF
!IF "$(CFG)" == "ARM\Release" || "$(CFG)" == "ARM\Debug"
COMMON_ICUDATA_ARGUMENTS=$(COMMON_ICUDATA_ARGUMENTS) -a ARM
!ENDIF
!IF "$(CFG)" == "ARM64\Release" || "$(CFG)" == "ARM64\Debug"
COMMON_ICUDATA_ARGUMENTS=$(COMMON_ICUDATA_ARGUMENTS) -a ARM64
!ENDIF

#############################################################################
#
# ALL
#     This target builds all the data files.  The world starts here.
#			Note: we really want the common data dll to go to $(DLL_OUTPUT), not $(ICUBLD_PKG).  But specifying
#				that here seems to cause confusion with the building of the stub library of the same name.
#				Building the common dll in $(ICUBLD_PKG) unconditionally copies it to $(DLL_OUTPUT) too.
#
#############################################################################
!IF "$(ARM_CROSS_BUILD)" == ""
ALL : GODATA "$(ICU_LIB_TARGET)" "$(TESTDATAOUT)\testdata.dat"
	@echo All targets are up to date

!IF "$(UWP)" == "UWP"
	@if not exist "$(ICUMAKE)\..\..\commondata\" mkdir "$(ICUMAKE)\..\..\commondata\"
    copy "$(ICUOUT)\$(U_ICUDATA_NAME)$(U_ICUDATA_ENDIAN_SUFFIX).dat" "$(ICUMAKE)\..\..\commondata\"
!ENDIF

!ELSE
ALL : GODATA "$(ICU_LIB_TARGET)" "$(TESTDATAOUT)\testdata.dat" $(ARM_CROSSBUILD_TS)
	@echo All targets are up to date

!ENDIF

# Verbose output when building the data for Debug builds.
!IF "$(DEBUG)" == "true"
ICU_DATA_BUILD_VERBOSE=--verbose
!ELSE
ICU_DATA_BUILD_VERBOSE=
!ENDIF

# Three main targets: tools, core data, and test data.
# Keep track of whether they are built via timestamp files.

$(TOOLS_TS): "$(ICUTOOLS)\genrb\$(CFGTOOLS)\genrb.exe" "$(ICUTOOLS)\gencnval\$(CFGTOOLS)\gencnval.exe" "$(ICUTOOLS)\gencfu\$(CFGTOOLS)\gencfu.exe" "$(ICUTOOLS)\icupkg\$(CFGTOOLS)\icupkg.exe" "$(ICUTOOLS)\makeconv\$(CFGTOOLS)\makeconv.exe" "$(ICUPBIN)\pkgdata.exe"
	@echo "timestamp" > $(TOOLS_TS)

# On Unix, Python generates at configure time a list of Makefile rules.
# On Windows, however, we run the Python data build script at build time instead.
# The alternative would be to use a preprocessor macro to generate rules for nmake.
# However, this approach was abandoned for reasons including:
#
#  - nmake imposes more stringent restrictions on command line length.
#  - The lack of gnumake features makes nmake file construction more complex.
#  - Windows builds are single-threaded, giving less advantage to a Makefile approach.
#
# Currently, the entire script needs to run even for small changes to data. Maybe consider
# checking file-changed timestamps in Python to build only the required subset of data.

$(COREDATA_TS):
	@cd "$(ICUSRCDATA)"
	set PYTHONPATH=$(ICUP)\source\python;%PYTHONPATH%
	py -3 -B -m icutools.databuilder \
		--mode windows-exec \
		--src_dir "$(ICUSRCDATA)" \
		--tool_dir "$(ICUTOOLS)" \
		--tool_cfg "$(CFGTOOLS)" \
		--out_dir "$(ICUBLD_PKG)" \
		--tmp_dir "$(ICUTMP)" \
		--filter_file "$(ICU_DATA_FILTER_FILE)" \
		$(ICU_DATA_BUILD_VERBOSE) \
		$(ICU_DATA_BUILDTOOL_OPTS)
	@echo "timestamp" > $(COREDATA_TS)

	
# The core Unicode properties files (uprops.icu, ucase.icu, ubidi.icu)
# are hardcoded in the common DLL and therefore not included in the data package any more.
# They are not built by default but need to be built for ICU4J data and for getting the .c source files
# when updating the Unicode data.
# Changed in makedata.mak revision 1.117. See Jitterbug 4497.
# 2010-dec Removed pnames.icu.
# Command line:
#   C:\svn\icuproj\icu\trunk\source\data>nmake -f makedata.mak ICUMAKE=C:\svn\icuproj\icu\trunk\source\data\ CFG=x86\Debug uni-core-data
uni-core-data: GODATA "$(ICUBLD_PKG)\pnames.icu" "$(ICUBLD_PKG)\uprops.icu" "$(ICUBLD_PKG)\ucase.icu" "$(ICUBLD_PKG)\ubidi.icu" "$(ICUBLD_PKG)\nfc.nrm"
	@echo Unicode .icu files built to "$(ICUBLD_PKG)"

# Build the ICU4J icudata.jar and testdata.jar.
# see icu4j-readme.txt

ICU4J_TZDATA="$(ICUOUT)\icu4j\icutzdata.jar"
ICU4J_DATA_DIRNAME=com\ibm\icu\impl\data\$(U_ICUDATA_NAME)b
ICU4J_TZDATA_PATHS=$(ICU4J_DATA_DIRNAME)\zoneinfo64.res $(ICU4J_DATA_DIRNAME)\metaZones.res $(ICU4J_DATA_DIRNAME)\timezoneTypes.res $(ICU4J_DATA_DIRNAME)\windowsZones.res

generate-data: GODATA "$(ICUOUT)\$(ICUPKG).dat" uni-core-data
	if not exist "$(ICUOUT)\icu4j\$(ICU4J_DATA_DIRNAME)" mkdir "$(ICUOUT)\icu4j\$(ICU4J_DATA_DIRNAME)"
	if not exist "$(ICUOUT)\icu4j\tzdata\$(ICU4J_DATA_DIRNAME)" mkdir "$(ICUOUT)\icu4j\tzdata\$(ICU4J_DATA_DIRNAME)"
	echo pnames.icu ubidi.icu ucase.icu uprops.icu nfc.nrm > "$(ICUOUT)\icu4j\add.txt"
	"$(ICUPBIN)\icupkg" "$(ICUOUT)\$(ICUPKG).dat" "$(ICUOUT)\icu4j\$(U_ICUDATA_NAME)b.dat" -a "$(ICUOUT)\icu4j\add.txt" -s "$(ICUBLD_PKG)" -x * -tb -d "$(ICUOUT)\icu4j\$(ICU4J_DATA_DIRNAME)"
	@for %f in ($(ICU4J_TZDATA_PATHS)) do @move "$(ICUOUT)\icu4j\%f" "$(ICUOUT)\icu4j\tzdata\$(ICU4J_DATA_DIRNAME)"

"$(ICUOUT)\icu4j\icutzdata.jar": GODATA generate-data
	"$(JAR)" cf "$(ICUOUT)\icu4j\icutzdata.jar" -C "$(ICUOUT)\icu4j\tzdata" "$(ICU4J_DATA_DIRNAME)"

# Build icudata.jar:
# - add the uni-core-data to the ICU package
# - swap the ICU data
# - extract all data items
# - package them into the .jar file
"$(ICUOUT)\icu4j\icudata.jar": GODATA generate-data
	"$(JAR)" cf "$(ICUOUT)\icu4j\icudata.jar" -C "$(ICUOUT)\icu4j" "$(ICU4J_DATA_DIRNAME)"

# Build testdata.jar:
# - swap the test data
# - extract all data items
# - package them into the .jar file
"$(ICUOUT)\icu4j\testdata.jar": GODATA "$(TESTDATAOUT)\testdata.dat"
	if not exist "$(ICUOUT)\icu4j\com\ibm\icu\dev\data\testdata" mkdir "$(ICUOUT)\icu4j\com\ibm\icu\dev\data\testdata"
	"$(ICUPBIN)\icupkg" "$(TESTDATAOUT)\testdata.dat" -r test.icu -x * -tb -d "$(ICUOUT)\icu4j\com\ibm\icu\dev\data\testdata"
	"$(JAR)" cf "$(ICUOUT)\icu4j\testdata.jar" -C "$(ICUOUT)\icu4j" com\ibm\icu\dev\data\testdata

## Compare to:  source\data\Makefile.in and source\test\testdata\Makefile.in

DEBUGUTILITIESDATA_DIR=main\tests\core\src\com\ibm\icu\dev\test\util
DEBUGUTILITIESDATA_SRC=DebugUtilitiesData.java

# Build DebugUtilitiesData.java
"$(ICUOUT)\icu4j\src\$(DEBUGUTILITIESDATA_DIR)\$(DEBUGUTILITIESDATA_SRC)" : {"$(ICUTOOLS)\gentest\$(CFGTOOLS)"}gentest.exe
	if not exist "$(ICUOUT)\icu4j\src\$(DEBUGUTILITIESDATA_DIR)" mkdir "$(ICUOUT)\icu4j\src\$(DEBUGUTILITIESDATA_DIR)"
	"$(ICUTOOLS)\gentest\$(CFGTOOLS)\gentest" -j -d"$(ICUOUT)\icu4j\src\$(DEBUGUTILITIESDATA_DIR)"

ICU4J_DATA="$(ICUOUT)\icu4j\icudata.jar" "$(ICUOUT)\icu4j\testdata.jar"  "$(ICUOUT)\icu4j\src\$(DEBUGUTILITIESDATA_DIR)\$(DEBUGUTILITIESDATA_SRC)"

icu4j-data: GODATA $(ICU4J_DATA) $(ICU4J_TZDATA)

!IFDEF ICU4J_ROOT

"$(ICU4J_ROOT)\main\shared\data\icudata.jar": "$(ICUOUT)\icu4j\icudata.jar"
	if not exist "$(ICU4J_ROOT)\main\shared\data" mkdir "$(ICU4J_ROOT)\main\shared\data"
	copy "$(ICUOUT)\icu4j\icudata.jar" "$(ICU4J_ROOT)\main\shared\data"

"$(ICU4J_ROOT)\main\shared\data\icutzdata.jar": "$(ICUOUT)\icu4j\icutzdata.jar"
	if not exist "$(ICU4J_ROOT)\main\shared\data" mkdir "$(ICU4J_ROOT)\main\shared\data"
	copy "$(ICUOUT)\icu4j\icutzdata.jar" "$(ICU4J_ROOT)\main\shared\data"

"$(ICU4J_ROOT)\main\shared\data\testdata.jar": "$(ICUOUT)\icu4j\testdata.jar"
	if not exist "$(ICU4J_ROOT)\main\shared\data" mkdir "$(ICU4J_ROOT)\main\shared\data"
	copy "$(ICUOUT)\icu4j\testdata.jar" "$(ICU4J_ROOT)\main\shared\data"

# "$(DEBUGUTILTIESDATA_OUT)"

"$(ICU4J_ROOT)\$(DEBUGUTILITIESDATA_DIR)\$(DEBUGUTILITIESDATA_SRC)": "$(ICUOUT)\icu4j\src\$(DEBUGUTILITIESDATA_DIR)\$(DEBUGUTILITIESDATA_SRC)"
	if not exist "$(ICU4J_ROOT)\$(DEBUGUTILITIESDATA_DIR)" mkdir "$(ICU4J_ROOT)\$(DEBUGUTILITIESDATA_DIR)"
	copy "$(ICUOUT)\icu4j\src\$(DEBUGUTILITIESDATA_DIR)\$(DEBUGUTILITIESDATA_SRC)" "$(ICU4J_ROOT)\$(DEBUGUTILITIESDATA_DIR)\$(DEBUGUTILITIESDATA_SRC)"

ICU4J_DATA_INSTALLED="$(ICU4J_ROOT)\main\shared\data\icudata.jar" "$(ICU4J_ROOT)\main\shared\data\icutzdata.jar" "$(ICU4J_ROOT)\main\shared\data\testdata.jar" "$(ICU4J_ROOT)\$(DEBUGUTILITIESDATA_DIR)\$(DEBUGUTILITIESDATA_SRC)"

icu4j-data-install : GODATA $(ICU4J_DATA) $(ICU4J_TZDATA) $(ICU4J_DATA_INSTALLED)
	@echo ICU4J  data output to "$(ICU4J_ROOT)"

!ELSE

icu4j-data-install : 
	@echo ERROR ICU4J_ROOT not set
	@exit 1

!ENDIF



#
# testdata - nmake will invoke pkgdata, which will create testdata.dat
#
"$(TESTDATAOUT)\testdata.dat": "$(TESTDATA)\*" $(TOOLS_TS)
	@cd "$(TESTDATA)"
	@echo building testdata...
	nmake /nologo /f "$(TESTDATA)\testdata.mak" TESTDATA=. ICUTOOLS="$(ICUTOOLS)" ICUPBIN="$(ICUPBIN)" ICUP="$(ICUP)" CFG=$(CFGTOOLS) TESTDATAOUT="$(TESTDATAOUT)" TESTDATABLD="$(TESTDATABLD)" ICUSRCDATA="$(ICUSRCDATA)" DLL_OUTPUT="$(DLL_OUTPUT)"

#invoke pkgdata for ICU common data
#  pkgdata will drop all output files (.dat, .dll, .lib) into the target (ICUBLD_PKG) directory.
#  move the .dll and .lib files to their final destination afterwards.
#  The $(U_ICUDATA_NAME).lib and $(U_ICUDATA_NAME).exp should already be in the right place due to stubdata.
#
#  2005-may-05 Removed Unicode properties files (unorm.icu, uprops.icu, ucase.icu, ubidi.icu)
#  from data build. See Jitterbug 4497. (makedata.mak revision 1.117)
#
!IFDEF ICUDATA_SOURCE_ARCHIVE
"$(ICU_LIB_TARGET)" : $(COMMON_ICUDATA_DEPENDENCIES) "$(ICUDATA_SOURCE_ARCHIVE)"
	@echo Building icu data from $(ICUDATA_SOURCE_ARCHIVE)
	cd "$(ICUBLD_PKG)"
	"$(ICUPBIN)\icupkg" -x * --list "$(ICUDATA_SOURCE_ARCHIVE)" > "$(ICUTMP)\icudata.lst"
	"$(ICUPBIN)\pkgdata" $(COMMON_ICUDATA_ARGUMENTS) "$(ICUTMP)\icudata.lst"
	copy "$(U_ICUDATA_NAME).dll" "$(DLL_OUTPUT)"
	-@erase "$(U_ICUDATA_NAME).dll"
	copy "$(ICUTMP)\$(ICUPKG).dat" "$(ICUOUT)\$(U_ICUDATA_NAME)$(U_ICUDATA_ENDIAN_SUFFIX).dat"
	-@erase "$(ICUTMP)\$(ICUPKG).dat"
!ELSE
"$(ICU_LIB_TARGET)" : $(COMMON_ICUDATA_DEPENDENCIES) $(COREDATA_TS)
	@echo Building ICU data from scratch
	cd "$(ICUBLD_PKG)"
	"$(ICUPBIN)\pkgdata" $(COMMON_ICUDATA_ARGUMENTS) $(ICUTMP)\icudata.lst
	-@erase "$(ICU_LIB_TARGET)"
	@if not exist "$(DLL_OUTPUT)" mkdir "$(DLL_OUTPUT)"
	copy "$(U_ICUDATA_NAME).dll" "$(ICU_LIB_TARGET)"
	-@erase "$(U_ICUDATA_NAME).dll"
	copy "$(ICUTMP)\$(ICUPKG).dat" "$(ICUOUT)\$(U_ICUDATA_NAME)$(U_ICUDATA_ENDIAN_SUFFIX).dat"
	-@erase "$(ICUTMP)\$(ICUPKG).dat"
!ENDIF

"$(ARM_CROSSBUILD_TS)" : $(COMMON_ICUDATA_DEPENDENCIES) "$(ICU_LIB_TARGET)"
	@echo Building ICU data for "$(ARM_CROSS_BUILD)" from x64
	cd "$(ICUBLD_PKG)"
	"$(ICUPBIN)\pkgdata" $(COMMON_ICUDATA_ARGUMENTS) $(ICUTMP)\icudata.lst
	-@erase "$(ICU_LIB_TARGET)"
	@if not exist "$(DLL_OUTPUT)" mkdir "$(DLL_OUTPUT)"
	copy "$(U_ICUDATA_NAME).dll" "$(ICU_LIB_TARGET)"
	-@erase "$(U_ICUDATA_NAME).dll"
	copy "$(ICUTMP)\$(ICUPKG).dat" "$(ICUOUT)\$(U_ICUDATA_NAME)$(U_ICUDATA_ENDIAN_SUFFIX).dat"
	-@erase "$(ICUTMP)\$(ICUPKG).dat"
	@echo "timestamp" > $(ARM_CROSSBUILD_TS)

# utility target to create missing directories
# Most directories are made by Python, but still create ICUTMP
# so it works in the source archive
CREATE_DIRS :
	@if not exist "$(ICUOUT)\$(NULL)" mkdir "$(ICUOUT)"
	@if not exist "$(ICUTMP)\$(NULL)" mkdir "$(ICUTMP)"
	@if not exist "$(ICUOUT)\build\$(NULL)" mkdir "$(ICUOUT)\build"
	@if not exist "$(ICUBLD_PKG)\$(NULL)" mkdir "$(ICUBLD_PKG)"
	@if not exist "$(TESTDATAOUT)" mkdir "$(TESTDATAOUT)"

# utility target to send us to the right dir
GODATA : CREATE_DIRS
	@cd "$(ICUBLD_PKG)"

# This is to remove all the data files
CLEAN : GODATA
	@echo Cleaning up the data files.
	@cd "$(ICUOUT)"
	-@erase "$(ICUOUT)\*.dat"
	@rmdir $(ICUBLD) /s /q
	@rmdir $(ICUTMP) /s /q
	@rmdir $(TESTDATAOUT) /s /q

# DLL version information
# If you modify this, modify winmode.c in pkgdata.
"$(ICUTMP)\icudata.res": "$(ICUSRCDATA)\misc\icudata.rc"
	@echo Creating data DLL version information from $**
	@rc.exe /i "..\..\..\..\common" /r /fo $@ $**

# Targets for prebuilt Unicode data
# Needed for ICU4J!
"$(ICUBLD_PKG)\pnames.icu": $(ICUSRCDATA_RELATIVE_PATH)\in\pnames.icu
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\ubidi.icu": $(ICUSRCDATA_RELATIVE_PATH)\in\ubidi.icu
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\ucase.icu": $(ICUSRCDATA_RELATIVE_PATH)\in\ucase.icu
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\uprops.icu": $(ICUSRCDATA_RELATIVE_PATH)\in\uprops.icu
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\unames.icu": $(ICUSRCDATA_RELATIVE_PATH)\in\unames.icu
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\nfc.nrm": $(ICUSRCDATA_RELATIVE_PATH)\in\nfc.nrm
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\nfkc.nrm": $(ICUSRCDATA_RELATIVE_PATH)\in\nfkc.nrm
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\nfkc_cf.nrm": $(ICUSRCDATA_RELATIVE_PATH)\in\nfkc_cf.nrm
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\uts46.nrm": $(ICUSRCDATA_RELATIVE_PATH)\in\uts46.nrm
	"$(ICUPBIN)\icupkg" -tl $? $@

"$(ICUBLD_PKG)\coll\ucadata.icu": $(ICUSRCDATA_RELATIVE_PATH)\in\coll\ucadata-unihan.icu
	"$(ICUPBIN)\icupkg" -tl $? $@


!IFDEF ICUDATA_ARCHIVE
"$(ICUDATA_SOURCE_ARCHIVE)": CREATE_DIRS $(ICUDATA_ARCHIVE) $(TOOLS_TS)
	"$(ICUTOOLS)\icupkg\$(CFGTOOLS)\icupkg" -t$(U_ICUDATA_ENDIAN_SUFFIX) "$(ICUDATA_ARCHIVE)" "$(ICUDATA_SOURCE_ARCHIVE)"
!ENDIF
