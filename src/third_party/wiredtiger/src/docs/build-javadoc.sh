DOCS=`dirname $0`
TOP=$DOCS/..
. $TOP/config.sh

CLASSPATH=$THRIFT_HOME/libthrift.jar:$SLF4J_JAR javadoc -public -d $DOCS/java \
	-source 1.5 \
	-sourcepath $TOP/lang/java/src:$TOP/src/server/gen-java \
	-stylesheetfile $DOCS/style/javadoc.css \
	-use -link http://java.sun.com/j2se/1.5.0/docs/api/ \
	-header '<b>WiredTiger API</b><br><font size="-1"> version '$WT_VERSION'</font>' \
	-windowtitle 'WiredTiger Java API' -bottom '<font size=1>Copyright (c) 2008-2016 MongoDB, Inc.  All rights reserved.</font>' \
	com.wiredtiger com.wiredtiger.util
