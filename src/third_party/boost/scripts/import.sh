#!/bin/bash
# This script downloads and imports boost via the boost bcp utility.
# It can be run on Linux or Mac OS X.
# Actual integration into the build system is not done by this script.
#
# Turn on strict error checking, like perl use 'strict'
set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

BOOST_GIT_URL="https://github.com/mongodb-forks/boost.git"
VERSION=1.88.0
BOOST_GIT_BRANCH=v${VERSION}-mongo

# Exact list of boost libraries we directly and indirectly depend on.
BOOST_LIBS=(
    algorithm
    align
    any
    array
    asio
    assert
    assign
    atomic
    bind
    chrono
    compat
    concept_check
    config
    container
    container_hash
    context
    core
    coroutine
    date_time
    detail
    dynamic_bitset
    exception
    filesystem
    foreach
    format
    function
    function_types
    functional
    fusion
    graph
    integer
    interprocess
    intrusive
    io
    iostreams
    iterator
    lambda
    lexical_cast
    locale
    log
    logic
    math
    move
    mp11
    mpi
    mpl
    multi_index
    multiprecision
    numeric
    optional
    parameter
    phoenix
    predef
    preprocessor
    process
    program_options
    property_tree
    proto
    random
    range
    ratio
    rational
    regex
    scope
    serialization
    smart_ptr
    sort
    spirit
    system
    test
    thread
    timer
    tokenizer
    tuple
    type_index
    type_traits
    typeof
    unordered
    utility
    uuid
    variant
    vmd
    winapi
    xpressive
)

GIT_ROOT=$(git rev-parse --show-toplevel)
DEST_DIR=$GIT_ROOT/src/third_party/boost

# Import files from our fork of boost.
import() {
    LIB_GIT_DIR=$(mktemp -d /tmp/import-boost.XXXXXX)
    pushd $LIB_GIT_DIR
    trap "rm -rf $LIB_GIT_DIR" EXIT RETURN

    git clone "$BOOST_GIT_URL" -b $BOOST_GIT_BRANCH --recurse-submodules $LIB_GIT_DIR

    # Build the bcp tool
    # The bcp tool is a boost specific tool that allows importing a subset of boost
    # The downside is that it copies a lot of unnecessary stuff in libs
    # and does not understand #ifdefs
    ./bootstrap.sh
    ./b2 tools/bcp

    # Nuke the install dirs and use bcp to copy over the files.
    test -d $DEST_DIR || mkdir $DEST_DIR
    rm -rf $DEST_DIR/boost $DEST_DIR/libs
    ./dist/bin/bcp --boost=$PWD ${BOOST_LIBS[@]} $DEST_DIR

    popd
}
import

# Trim files.
trim() {
    pushd $DEST_DIR

    rm -f Jamroot boost.png rst.css
    rm -rf doc
    find libs -type f -name .gitignore -print0 | xargs -0 rm -f

    # Strip directories that exclusively contain non-source.
    STRIP_DIRS=(
        test
        doc
        build
        examples
        example
        meta
        tutorial
        performance
        bench
        perf
        xmldoc
        bug
    )
    for STRIP_DIR in ${STRIP_DIRS[@]}; do
        find boost -type d -name $STRIP_DIR -print0 | xargs -0 rm -rf
        find libs  -type d -name $STRIP_DIR -print0 | xargs -0 rm -rf
    done

    # Strip files of extensions that are non-source.
    STRIP_EXTS=(
        html
        htm
        png
        txt
        md
        jam
    )
    for STRIP_EXT in ${STRIP_EXTS[@]}; do
        find boost -name "*.$STRIP_EXT" -print0 | xargs -0 rm -f
        find libs  -name "*.$STRIP_EXT" -print0 | xargs -0 rm -f
    done

    # Remove compatibility files for compilers we do not support.
    STRIP_COMPILERS=(
        dmc
        bcc
        bcc551
        bcc_pre590
        mwcw
        msvc60
        msvc70
    )
    for STRIP_COMPILER in ${STRIP_COMPILERS[@]}; do
        find boost -type d -name $STRIP_COMPILER -print0 | xargs -0 rm -rf
        find libs  -type d -name $STRIP_COMPILER -print0 | xargs -0 rm -rf
    done

    # Remove includes under src/third_party/libs/*/include/boost that have exact
    # duplicates in src/third_party/boost/boost.
    for LIB_HEADER_DIR in libs/*/include/boost/*; do
        [[ "$LIB_HEADER_DIR" =~ ^libs/([^/]+)/include/boost/ ]]
        LIB_NAME="${BASH_REMATCH[1]}"
        LIB_HEADER=
        find $LIB_HEADER_DIR -type f -print0 | while read -d $'\0' LIB_HEADER; do
            TOP_LEVEL_HEADER=${LIB_HEADER/$LIB_HEADER_DIR/boost\/$LIB_NAME}
            [ -f "$TOP_LEVEL_HEADER" ] && cmp -s "$TOP_LEVEL_HEADER" "$LIB_HEADER" && rm "$LIB_HEADER"
        done || [[ -z $LIB_HEADER ]]
    done

    # Remove empty directories
    find . -type d -empty -print0 | xargs -0 rmdir

    popd
}
trim
