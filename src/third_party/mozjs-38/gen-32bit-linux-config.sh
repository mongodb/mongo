#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Please supply an arch: x86_64, x86, etc and a platform: osx, linux, windows, etc"
    exit 0;
fi

# the two files we need are js-confdefs.h which get used for the build and
# js-config.h for library consumers.  We also get different unity source files
# based on configuration, so save those too.

cd mozilla-release/js/src

PYTHON=python CCFLAGS="-m32" CFLAGS="-m32" LINKFLAGS="-m32" ./configure --without-intl-api --enable-posix-nspr-emulation --target=i686 --disable-trace-logging

cd ../../..

rm -rf platform/$1/$2/

mkdir -p platform/$1/$2/build
mkdir platform/$1/$2/include

cp mozilla-release/js/src/js/src/js-confdefs.h platform/$1/$2/build
cp mozilla-release/js/src/js/src/*.cpp platform/$1/$2/build
cp mozilla-release/js/src/js/src/js-config.h platform/$1/$2/include
