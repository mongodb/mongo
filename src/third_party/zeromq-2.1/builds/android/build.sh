#! /bin/bash
#
#   Does an NDK build by creating the necessary JNI directory
#   and copying the libzmq source files into a temporary tree.
#   We build a static uuid library (libuuid isn't provided on
#   Android) and link that into libzmq.so. After the build is
#   done, creates a symlink to the resulting libzmq.so.
#
#   The clean.sh script restores the libzmq environment to its
#   former state.

SRC_DIR="../../src/"
JNI_DIR="../../src/jni"
UUID_DIR="uuid"

NDK_BUILD=`which ndk-build`

if [ -z "$NDK_BUILD" ]; then
    echo "Must have ndk-build in your path"
    exit 1
fi

if [ ! -d $JNI_DIR ]; then
    mkdir $JNI_DIR
fi

if [ -d $UUID_DIR ]; then
    echo "Copying $UUID_DIR $JNI_DIR/$UUID_DIR"
    cp -r $UUID_DIR $JNI_DIR/$UUID_DIR
fi

for i in `ls $SRC_DIR`; do
    if [ -f $SRC_DIR/$i -a ! -f $JNI_DIR/$i ]; then
        echo "copying $SRC_DIR/$i --> $JNI_DIR/$i"
        cp $SRC_DIR/$i $JNI_DIR/$i
    fi
done

cp Android.mk $JNI_DIR;
cp Application.mk $JNI_DIR

pushd .
cd $JNI_DIR
echo "Current Working dir: `pwd`"
$NDK_BUILD
popd

if [ -f ../../src/libs/armeabi/libzmq.so ]; then
    ln -s ../../src/libs/armeabi/libzmq.so ./libzmq.so
fi

exit 0
