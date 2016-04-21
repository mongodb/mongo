#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Please supply an arch: x86_64, x86, etc and a platform: osx, linux, windows, etc"
    exit 0;
fi

# the two files we need are js-confdefs.h which get used for the build and
# js-config.h for library consumers.  We also get different unity source files
# based on configuration, so save those too.

#cd mozilla-release/js/src

cat mozconfig >>EOF
ac_add_options --without-intl-api
ac_add_options --enable-posix-nspr-emulation
ac_add_options --disable-trace-logging

ac_add_options --target=x86_64-pc-mingw32
ac_add_options --host=x86_64-pc-mingw32
EOF
#./mach build

#cd ../../..

rm -rf platform/$1/$2/

mkdir -p platform/$1/$2/build
mkdir platform/$1/$2/include

#MOZBUILD=/z/moz/mozilla-central/obj-x86_64-pc-mingw32

MOZBUILD=/z/moz/mozilla-central/obj-i686-pc-mingw32


cp $MOZBUILD/js/src/js-confdefs.h platform/$1/$2/build
cp $MOZBUILD/js/src/*.cpp platform/$1/$2/build
cp $MOZBUILD/js/src/js-config.h platform/$1/$2/include
