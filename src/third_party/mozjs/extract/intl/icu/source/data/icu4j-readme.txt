* Copyright (C) 2016 and later: Unicode, Inc. and others.
* License & terms of use: http://www.unicode.org/copyright.html
********************************************************************************
* Copyright (C) 2008-2012, International Business Machines Corporation         *
* and others. All Rights Reserved.                                             *
*                                                                              *
* 6/26/08 - Created by Brian Rower - heavy copying from ICU4J readme & others  *
*                                                                              *
********************************************************************************

Procedures for building ICU4J data from ICU4C data:

*Setup*

In the following,
        $icu4c_root is the ICU4C root directory
        $icu4j_root is the ICU4J root directory
        $jdk_bin is the JDK bin directory (for the jar tool)

1. *Linux*
   Download, configure, and build ICU4C. When you configure ICU4C, you must
   set the environment variable ICU_DATA_BUILDTOOL_OPTS to
   "--include_uni_core_data" to build additional required ICU4J data:

        ICU_DATA_BUILDTOOL_OPTS=--include_uni_core_data ./runConfigureICU Linux
   
   *Windows*
    - Add ICU_DATA_BUILDTOOL_OPTS as a system environment variable with value "--include_uni_core_data"
    - Build $icu4c_root\source\allinone\allinone.sln in Visual Studio

   For more instructions on downloading and building ICU4C,
   see the ICU4C readme at:
        https://htmlpreview.github.io/?https://github.com/unicode-org/icu/blob/master/icu4c/readme.html#HowToBuild
        (Windows: build as 'x86, Release' otherwise you will have to set 'CFG' differently below.)

    *NOTE* You should do a full rebuild after any data changes.
    
1a.  If you didn't download from GitHub, you will also need the "icu4c-*-data.zip" file.  Unpack this file and replace the icu/source/data directory's contents with the contents of the data directory from the zip file.  


2. Step 2 depends on whether you are on a Windows or a Unix-type
platform.

*Windows* 

2a. On Developer Command Prompt for VS, cd to $icu4c_root\source\data.

2b. On Developer Command Prompt for VS,
        nmake -f makedata.mak ICUMAKE=$icu4c_root\source\data\ CFG=x86\Release JAR="$jdk_bin\jar" ICU4J_ROOT=$icu4j_root icu4j-data-install

       Continue with step 3 below, in Java:


*Linux*

        $icu4c_build is the ICU4C root build directory,
        which is $icu4c_root/source in an in-source build.
        (in other words, $icu4c_build is where you ran runConfigureICU or configure)

2c. On the command line, cd to $icu4c_build/data

2d. Do
        make JAR=$jdk_bin/jar ICU4J_ROOT=$icu4j_root icu4j-data-install

       (You can omit the JAR if it's just jar.)

	Continue with step 3, in Java:

Step 2 on either platform will produce two files: icudata.jar and
icutzdata.jar in $icu4j_root/main/shared/data.

*Java*

3. After the ICU4C-side steps above, build the main target of the
        ICU4J ant build to unpack the jar files with the following commands:

        cd $icu4j_root
        ant main

   Run the tests locally and make sure they pass:
   
        ant check
