#!/bin/sh

TOP=`dirname $0`
TOP=`cd $TOP && /bin/pwd`

. $TOP/config.sh

BUILD=$TOP/build
mkdir -p $BUILD

# Core API
gcc -Iinclude -c src/api/api.c -o $BUILD/api.o
gcc -Iinclude -c src/api/cur_std.c -o $BUILD/cur_std.o
gcc -Iinclude -c src/api/pack.c -o $BUILD/pack.o
g++ -Iinclude -I$DB_HOME -c src/bdb/bdb.cpp -o $BUILD/bdb.o

rm -f $BUILD/libwt.a
ar -q $BUILD/libwt.a $BUILD/api.o $BUILD/cur_std.o $BUILD/pack.o $BUILD/bdb.o

# RPC server
cd $TOP/src/server
g++ -O -Igen-cpp -I$THRIFT_HOME/include/thrift -Iinclude -I../../include -o $BUILD/wt_server server.cpp gen-cpp/wiredtiger_*.cpp $BUILD/libwt.a $THRIFT_HOME/lib/libthrift.a

# Examples
cd $TOP/examples/c
make

# Java API
cd $TOP
CLASSES=lang/java/classes
mkdir -p $CLASSES
CLASSPATH=$THRIFT_HOME/libthrift.jar:$SLF4J_JAR javac -d $CLASSES src/server/gen-java/com/wiredtiger/protocol/W*.java lang/java/src/com/wiredtiger/*.java lang/java/src/com/wiredtiger/*/*.java lang/java/tests/com/wiredtiger/*.java
