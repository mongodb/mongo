#!/bin/bash

cd $(git rev-parse --show-toplevel)
echo `pwd`
sh build_posix/reconf

curdir=`pwd`

flags="CFLAGS=\"-Werror -Wall -Wextra -Waddress -Waggregate-return -Wbad-function-cast -Wcast-align -Wdeclaration-after-statement -Wformat-security -Wformat-nonliteral -Wformat=2 -Wmissing-declarations -Wmissing-field-initializers -Wmissing-prototypes -Wnested-externs -Wno-unused-parameter -Wpointer-arith -Wredundant-decls -Wshadow -Wundef -Wunused -Wwrite-strings -O -fno-strict-aliasing -Wuninitialized\"
CC=clang CFLAGS=\"-Wall -Werror -Qunused-arguments -Wno-self-assign -Wno-parentheses-equality -Wno-array-bounds\""

options="--enable-diagnostic
--disable-shared
--disable-static --enable-python
--enable-snappy --enable-zlib --enable-lz4
--with-builtins=lz4,snappy,zlib
--enable-diagnostic --enable-python
--enable-strict --disable-shared"

saved_IFS=$IFS
cr_IFS="
"

# This function may alter the current directory on failure
BuildTest() {
        extra_config=--enable-silent-rules
        echo "Building: $1, $2"
        rm -rf ./build || return 1
        mkdir build || return 1
        cd ./build
        eval ../configure $extra_config "$1" "$2" \
                 --prefix="$insdir" || return 1
        eval make "$3" || return 1
        make -C examples/c check VERBOSE=1 > /dev/null || return 1
        case "$2" in
                # Skip the install step with Python.  Even with --prefix, the
                # install tries to write to /usr/lib64/python2.7/site-packages .
                *enable-python* )  doinstall=false;;
                # Non-shared doesn't yet work: library is not found at link step (??)
                *disable-shared* ) doinstall=false;;
                * )                doinstall=true;;
        esac
        if $doinstall; then
                eval make install || return 1
                cflags=`pkg-config wiredtiger --cflags --libs`
                [ "$1"  == "CC=clang" ] && compiler="clang" || compiler="cc"
                echo $compiler -o ./smoke ../examples/c/ex_smoke.c $cflags
                $compiler -o ./smoke ../examples/c/ex_smoke.c  $cflags|| return 1
                LD_LIBRARY_PATH=$insdir/lib ./smoke || return 1
        fi
        return 0
}

ecode=0
insdir=`pwd`/installed
export PKG_CONFIG_PATH=$insdir/lib/pkgconfig
IFS="$cr_IFS"
for flag in $flags ; do
        for option in $options ; do
               cd "$curdir"
               IFS="$saved_IFS"
               if ! BuildTest "$flag" "$option" "$@"; then
                       ecode=1
                       echo "*** ERROR: $flag, $option"
               fi
               IFS="$cr_IFS"
       done
done
IFS=$saved_IFS
exit $ecode
