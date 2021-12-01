#!/bin/sh

set -e

cd $SOURCE
./mach configure
./mach build export
./mach build -X nsprpub mfbt memory memory/mozalloc modules/zlib mozglue js/src xpcom/glue js/ductwork/debugger js/ipc js/xpconnect/loader js/xpconnect/wrappers js/xpconnect/src
status=$?
echo "[[[[ build.js complete, exit code $status ]]]]"
exit $status
