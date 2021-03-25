#!/bin/bash
# Script to setup charybdefs
set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

echo Start - charybdefs_setup.sh

cd /data

rm -rf /data/charybdefs
rm -rf /data/thrift

# Use the mongo branch and fork from here
git clone -b mongo_42 https://github.com/markbenvenuto/charybdefs.git

# Run the build script in the mongo branch
cd charybdefs/mongo

# Build and setup thrift and charybdefs
PATH=/opt/mongodbtoolchain/v3/bin:$PATH bash ./build.sh

echo Done - charybdefs_setup.sh
