# Copyright (C) 2016 and later: Unicode, Inc. and others.
# License & terms of use: http://www.unicode.org/copyright.html
#  ***********************************************************************
#  * COPYRIGHT:
#  * Copyright (c) 2004-2006, International Business Machines Corporation
#  * and others. All Rights Reserved.
#  ***********************************************************************
#
# This perl script checks for correct memory function usage in ICU library code.
# It works with Linux builds of ICU using clang or gcc.
#
#  To run it,
#    1.  Build ICU
#    2.  cd  icu/source
#    3.  perl tools/memcheck/ICUMemCheck.pl
#
#  All object files containing direct references to C or C++ runtime library memory
#    functions will be listed in the output.
#
#  For ICU 58, the expected output is
#    common/uniset.o          U operator delete(void*)
#    common/unifilt.o         U operator delete(void*)
#    common/cmemory.o         U malloc
#    common/cmemory.o         U free
#    i18n/strrepl.o           U operator delete(void*)
#
#  cmemory.c          Expected failures from uprv_malloc, uprv_free implementation.
#  uniset.cpp         Fails because of SymbolTable::~SymbolTable()
#  unifilt.cpp        Fails because of UnicodeMatcher::~UnicodeMatcher()
#  strrepl.cpp        Fails because of UnicodeReplacer::~UnicodeReplacer()
#
#  To verify that no additional problems exist in the .cpp files, #ifdef out the
#  offending destructors, rebuild icu, and re-run the tool.  The problems should
#  be gone.
#
#  The problem destructors all are for mix-in style interface classes.
#  These classes can not derive from UObject or UMemory because of multiple-inheritance
#  problems, so they don't get the ICU memory functions.  The delete code
#  in the destructors will never be called because stand-alone instances of
#  the classes cannot exist.
#
$fileNames = `find common i18n io -name "*.o" -print`;
foreach $f (split('\n', $fileNames)) {
   $symbols = `nm -u -C $f`;
   if ($symbols =~ /U +operator delete\(void\*\)/) {
      print "$f 	$&\n";
   }
   if ($symbols =~ /U +operator delete\[\]\(void\*\)/) {
      print "$f 	$&\n";
   }
   if ($symbols =~ /U +operator new\(unsigned int\)/) {
      print "$f 	$&\n";
   }
   if ($symbols =~ /U +operator new\[\]\(unsigned int\)/) {
      print "$f 	$&\n";
   }
   if ($symbols =~ /U +malloc.*/) {
      print "$f 	$&\n";
   }
   if ($symbols =~ /(?m:U +free$)/) {
      print "$f 	$&\n";
   }

}
