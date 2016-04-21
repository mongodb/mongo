#!/bin/sh

if [ $# -ne 2 ]
then
    echo "Please supply an arch: x86_64, i386, etc and a platform: osx, linux, windows, etc"
    exit 0;
fi

# the two files we need are js-confdefs.h which get used for the build and
# js-config.h for library consumers.  We also get different unity source files
# based on configuration, so save those too.

cd mozilla-release/js/src

PYTHON=python ./configure --without-intl-api --enable-posix-nspr-emulation --disable-trace-logging

cd ../../..

rm -rf platform/$1/$2/

mkdir -p platform/$1/$2/build
mkdir platform/$1/$2/include

cp mozilla-release/js/src/js/src/js-confdefs.h platform/$1/$2/build
cp mozilla-release/js/src/js/src/*.cpp platform/$1/$2/build
cp mozilla-release/js/src/js/src/js-config.h platform/$1/$2/include

for unified_file in $(ls -1 platform/$1/$2/build/*.cpp) ; do
	sed 's/#include ".*\/js\/src\//#include "/' < $unified_file > t1
	sed 's/#error ".*\/js\/src\//#error "/' < t1 > $unified_file
	rm t1
done


