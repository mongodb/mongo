#!/bin/bash

# This script will create the docker container if not present and then launch it executing gdb with the given core.

[[ `pwd` = */tools/antithesis ]] || {
    echo "You must execute this script from the tools/antithesis directory"
    exit 1
}

sudo docker image ls | grep wt-test-format > /dev/null 2>&1
[[ $? -ne 0 ]] && {
    sudo docker build -f test_format.dockerfile -t wt-test-format:latest ../..
}

SRC=$1
[[ $SRC = /* ]] || SRC=`pwd`/$1

T_DIR=/opt/bin/test/format

sudo docker run --rm --mount type=bind,src=$SRC,dst=$T_DIR/core -it wt-test-format:latest /bin/bash -c "cd $T_DIR;gdb t core"
