#!/bin/bash
#
#  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
#
#  Distributed under the Boost Software License, Version 1.0.
#  https://www.boost.org/LICENSE_1_0.txt

# Small and Fast Cross Compile Script

rm -r build
cd build

BUILD_TYPE=i586-mingw32msvc
GETTEXT_VER=0.18.1.1
ICONV_VER=1.13.1

wget http://ftp.gnu.org/pub/gnu/libiconv/libiconv-$ICONV_VER.tar.gz || exit 1
wget http://ftp.gnu.org/pub/gnu/gettext/gettext-$GETTEXT_VER.tar.gz || exit 1

tar -xzf libiconv-$ICONV_VER.tar.gz || exit 1
tar -xzf gettext-$GETTEXT_VER.tar.gz || exit 1


mkdir win32
PACKAGE_DIR=gettext-tools-static-$GETTEXT_VER
mkdir $PACKAGE_DIR

PREFIX=`pwd`/win32

cd libiconv-$ICONV_VER

./configure --disable-shared --host=$BUILD_TYPE --prefix=$PREFIX || exit 1
make -j 4 && make install || exit 1
cp ./COPYING ../$PACKAGE_DIR/COPYING-libiconv.txt

cd ../gettext-$GETTEXT_VER

./configure --disable-shared --host=$BUILD_TYPE --prefix=$PREFIX --with-libiconv-prefix=$PREFIX || exit 1
cd gettext-tools
make -j 4 && make install || exit 1
cd ..

cp ./gettext-tools/gnulib-lib/libxml/COPYING ../$PACKAGE_DIR/COPYING-libxml.txt
cp ./COPYING ../$PACKAGE_DIR/COPYING-gettext.txt

cd ..

echo http://ftp.gnu.org/pub/gnu/libiconv/libiconv-$ICONV_VER.tar.gz > $PACKAGE_DIR/sources.txt
echo http://ftp.gnu.org/pub/gnu/gettext/gettext-$GETTEXT_VER.tar.gz >> $PACKAGE_DIR/sources.txt

cp win32/bin/*.exe $PACKAGE_DIR

unix2dos $PACKAGE_DIR/*.txt

zip $PACKAGE_DIR.zip $PACKAGE_DIR/*

mv $PACKAGE_DIR.zip ..
