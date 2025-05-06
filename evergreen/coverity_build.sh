#!/bin/env bash

set -eo pipefail

cd src

. evergreen/prelude_venv.sh
activate_venv
# number of parallel jobs to use for build.
# Even with scale=0 (the default), bc command adds decimal digits in case of multiplication. Division by 1 gives us a whole number with scale=0
parallel_jobs=$(bc <<< "$(grep -c '^processor' /proc/cpuinfo) * .85 / 1")

covIdir="$workdir/covIdir"
if [ -d "$covIdir" ]; then
  echo "covIdir already exists, meaning idir extracted after download from S3"
else
  mkdir $workdir/covIdir
fi
$workdir/coverity/bin/cov-configure --gcc
$workdir/coverity/bin/cov-build --dir "$covIdir" --verbose 0 --return-emit-failures --parse-error-threshold=99 python3 buildscripts/scons.py MONGO_VERSION="$version" --install-mode=hygienic install-mongod --install-action=hardlink --keep-going --variables-files=etc/scons/developer_versions.vars --variables-files=etc/scons/mongodbtoolchain_v4_gcc.vars --disable-warnings-as-errors -j $parallel_jobs --jlink=0.75 --opt=off --dbg=off --allocator=system --link-model=dynamic
ret=$?
if [ $ret -ne 0 ]; then
  echo "cov-build failed with exit code $ret"
else
  echo "cov-build was successful"
fi
