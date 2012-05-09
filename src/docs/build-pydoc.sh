DOCS=`dirname $0`
TOP=$DOCS/..
. $TOP/config.sh

cd python
PYTHONPATH=../../lang/python/src:$THRIFT_HOME/lib/python2.6/site-packages pydoc -w wiredtiger
