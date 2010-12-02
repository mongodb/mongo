DOCS=`dirname $0`
TOP=$DOCS/..
. $TOP/config.sh

javadoc -public -d $DOCS/java \
	-source 1.5 \
	-sourcepath $DOCS/../lang/java/src \
	-stylesheetfile $DOCS/style/javadoc.css \
	-use -link http://java.sun.com/j2se/1.5.0/docs/api/ \
	-header '<b>WiredTiger API</b><br><font size="-1"> version '$WT_VERSION'</font>' \
	-windowtitle 'WiredTiger Java API' -bottom '<font size=1>Copyright (c) 2010 WiredTiger.  All rights reserved.</font>' \
	com.wiredtiger com.wiredtiger.util
