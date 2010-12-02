#!/bin/sh

TOP=`dirname $0`
. $TOP/config.sh

cd $TOP/src/server
g++ -O -Igen-cpp -Iinclude -I../../include -o WiredTiger_server WiredTiger_server.cpp gen-cpp/wiredtiger_*.cpp ../api/api.c $THRIFT_HOME/lib/libthrift.a

cd ../..
CLASSES=lang/java/classes
mkdir -p $CLASSES
CLASSPATH=$THRIFT_HOME/libthrift.jar:$SLF4J_JAR javac -d $CLASSES src/server/gen-java/com/wiredtiger/protocol/W*.java lang/java/src/com/wiredtiger/*.java lang/java/src/com/wiredtiger/*/*.java 
