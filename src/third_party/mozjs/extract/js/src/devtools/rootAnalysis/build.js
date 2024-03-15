#!/bin/sh
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */


set -e

cd $SOURCE
./mach configure
./mach build export
./mach build -X nsprpub mfbt memory memory/mozalloc modules/zlib mozglue js/src xpcom/glue js/xpconnect/loader js/xpconnect/wrappers js/xpconnect/src
status=$?
echo "[[[[ build.js complete, exit code $status ]]]]"
exit $status
