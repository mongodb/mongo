#!/bin/sh
## -*-sh-*-
## Copyright (C) 2016 and later: Unicode, Inc. and others.
## License & terms of use: http://www.unicode.org/copyright.html
## Copyright (c) 2002, International Business Machines Corporation and
## others. All Rights Reserved.
#
# Just a script to test out icu-config.
#

set -x
which icu-config
icu-config
icu-config -?
icu-config --BAD ARGUMENT
icu-config --bindir               
icu-config --cflags               
icu-config --cxx
icu-config --cc
icu-config --cxxflags               
icu-config --cppflags               
icu-config --cppflags-searchpath
icu-config --incpath
icu-config --invoke
icu-config --invoke=genrb
icu-config --invoke=./myapp
icu-config --invoke=/path/to/myapp
icu-config --ldflags                 
icu-config --ldflags-searchpath
icu-config --ldflags-libsonly
icu-config --ldflags-system          
icu-config --ldflags-ustdio          
icu-config --exec-prefix               
icu-config --prefix               
icu-config --sbindir              
icu-config --sysconfdir
icu-config --mandir
icu-config --icudata
icu-config --icudatadir
icu-config --icudata-mode
icu-config --icudata-install-dir
icu-config --shared-datadir
icu-config --unicode-version      
icu-config --version              
# should fail
icu-config --prefix=/tmp --bindir
# following needs to point to an alternate path that will work 
icu-config --prefix=/Users/srl/II --cflags
icu-config --detect-prefix --ldflags 

