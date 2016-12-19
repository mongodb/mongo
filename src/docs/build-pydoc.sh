DOCS=`dirname $0`
TOP=$DOCS/..
. $TOP/config.sh

cd python
PYTHONPATH=../../lang/python/src:$THRIFT_HOME/lib/python2.7/site-packages pydoc -w wiredtiger
