#! /bin/bash
#
#   Restores the libzmq tree after a build.sh. Does no harm if
#   used more than once. Run from the builds/android directory.


SRC_DIR="../../src"
JNI_DIR="$SRC_DIR/jni"

if [ -d $JNI_DIR ]; then
    rm -rf $JNI_DIR
fi

if [ -d $SRC_DIR/obj ]; then
    rm -rf $SRC_DIR/obj
fi

if [ -d $SRC_DIR/libs ]; then
    rm -rf $SRC_DIR/libs
fi

if [ -h libzmq.so ]; then
    rm libzmq.so
fi

exit 0
