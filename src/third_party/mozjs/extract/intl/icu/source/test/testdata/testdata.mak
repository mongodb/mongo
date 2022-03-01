# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
#**********************************************************************
#* Copyright (C) 1999-2015, International Business Machines Corporation
#* and others.  All Rights Reserved.
#**********************************************************************
#
#   03/19/2001  weiv, schererm  Created

.SUFFIXES : .res .txt

TESTPKG=testdata
TESTDT=$(TESTPKG)


ALL : "$(TESTDATAOUT)\testdata.dat" 
	@echo Test data is built.

# old_l_testtypes.res is there for cintltst/udatatst.c/TestSwapData()
# I generated it with an ICU 4.2.1 build on Linux after removing
# testincludeUTF (which would make it large, unnecessarily for this test)
# and renaming the collations element to avoid build CollationElements
# (which will not work with a newer swapper)
# markus 2010jan15

# old_e_testtypes.res is the same, but icuswapped to big-endian EBCDIC

TESTDATATMP="$(TESTDATAOUT)\testdata"

CREATE_DIRS :
	@if not exist "$(TESTDATAOUT)\$(NULL)" mkdir "$(TESTDATAOUT)"
	@if not exist "$(TESTDATABLD)\$(NULL)" mkdir "$(TESTDATABLD)"
	@if not exist "$(TESTDATATMP)\$(NULL)" mkdir "$(TESTDATATMP)"

"$(TESTDATAOUT)\testdata.dat" :
	@echo Building test data
	set PYTHONPATH=$(ICUP)\source\python;%PYTHONPATH%
	py -3 -B -m icutools.databuilder \
		--mode windows-exec \
		--tool_dir "$(ICUTOOLS)" \
		--tool_cfg "$(CFG)" \
		--src_dir "$(TESTDATA)" \
		--tmp_dir "$(TESTDATATMP)" \
		--out_dir "$(TESTDATABLD)"
	"$(ICUPBIN)\pkgdata" -f -v -m common -c -p"$(TESTPKG)" -d "$(TESTDATAOUT)" -T "$(TESTDATABLD)" -s "$(TESTDATABLD)" $(TESTDATATMP)\testdata.lst
