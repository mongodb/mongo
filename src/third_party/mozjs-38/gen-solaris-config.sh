#!/bin/sh

# TODO:
# SpiderMonkey needs a newer version of python that omnios ships with. You'll need to build 2.7 yourself
# Also, the 64 bit build of firefox seems somewhat untested here, just use the interpreter.

if [ $# -ne 2 ]
then
    echo "Please supply an arch: x86_64, x86, etc and a platform: osx, linux, windows, etc"
    exit 0;
fi

# the two files we need are js-confdefs.h which get used for the build and
# js-config.h for library consumers.  We also get different unity source files
# based on configuration, so save those too.

cd mozilla-release/js/src

PYTHON=/home/mci/python/bin/python CC=/opt/mongodbtoolchain/bin/gcc CXX=/opt/mongodbtoolchain/bin/g++ CFLAGS=-m64 CCFLAGS=-m64 LINKFLAGS=-m64 -static-libstdc++ -static-libgcc OBJCOPY=/opt/mongodbtoolchain/bin/objcopy ./configure -without-intl-api --enable-posix-nspr-emulation --disable-ion

cd ../../..

rm -rf platform/$1/$2/

mkdir -p platform/$1/$2/build
mkdir platform/$1/$2/include

cp mozilla-release/js/src/js/src/js-confdefs.h platform/$1/$2/build
cp mozilla-release/js/src/js/src/*.cpp platform/$1/$2/build
cp mozilla-release/js/src/js/src/js-config.h platform/$1/$2/include
