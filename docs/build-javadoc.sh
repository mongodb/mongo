DOCS=`dirname $0`

javadoc -d $DOCS/java -public -sourcepath $DOCS/../lang/java/src com.wiredtiger com.wiredtiger.util
