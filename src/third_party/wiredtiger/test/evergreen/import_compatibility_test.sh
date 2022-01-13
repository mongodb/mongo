#!/usr/bin/env bash
#
# Test importing of files created in previous versions of WiredTiger.
# Test that we can downgrade a database after importing a file.

set -e

# build_branch --
#     1: branch
build_branch()
{
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Building branch: \"$1\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    # Clone if it doesn't already exist.
    if [ ! -d "$1" ]; then
        git clone --quiet https://github.com/wiredtiger/wiredtiger.git "$1"
    fi
    cd "$1"

    git checkout --quiet "$1"

    if [ "${build_sys[$1]}" == "cmake" ]; then
            . ./test/evergreen/find_cmake.sh
            config=""
            config+="-DENABLE_SNAPPY=1 "
            config+="-DWT_STANDALONE_BUILD=0 "
            (mkdir build && cd build &&
                $CMAKE $config ../. && make -j $(grep -c ^processor /proc/cpuinfo)) > /dev/null
    else
        config=""
        config+="--enable-snappy "
        config+="--disable-standalone-build "
        (mkdir build && cd build && sh ../build_posix/reconf &&
            ../configure $config && make -j $(grep -c ^processor /proc/cpuinfo)) > /dev/null
    fi
    cd ..
}

# create_file --
#     1: branch
#     2: file
create_file()
{
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Branch \"$1\" creating and populating \"$2\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    wt_cmd="$1/build/wt"
    test_dir="$1/build/WT_TEST/"
    uri="file:$2"

    # Make the home directory.
    mkdir -p $test_dir

    # Create the file and populate with a few key/values.
    $wt_cmd -h $test_dir create -c "key_format=S,value_format=S" $uri
    $wt_cmd -h $test_dir write $uri abc 123 def 456 hij 789
}

# import_file --
#     1: dest branch
#     2: source branch
#     3: file
import_file()
{
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Importing file \"$3\" from \"$1\" to \"$2\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    wt_cmd="$1/build/wt"
    test_dir="$1/build/WT_TEST/"
    mkdir -p $test_dir

    # Move the file across to the destination branch's home directory.
    import_file="$2/build/WT_TEST/$3"
    cp $import_file $test_dir

    # Run import via the wt tool.
    uri="file:$3"
    $wt_cmd -h $test_dir create -c "import=(enabled,repair=true)" $uri
}

# verify_file --
#     1: branch
#     2: file
verify_file()
{
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Branch \"$1\" verifying \"$2\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    wt_cmd="$1/build/wt"
    test_dir="$1/build/WT_TEST/"
    uri="file:$2"

    $wt_cmd -h $test_dir verify $uri
}

# cleanup_branch --
#     1: branch
cleanup_branch()
{
    test_dir="$1/build/WT_TEST/"
    if [ -d $test_dir ]; then
        rm -rf $test_dir
    fi
}

# import_compatibility_test --
#     1: newer branch
#     2: older branch
import_compatibility_test()
{
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Testing import compatibility between \"$1\" and \"$2\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    # Remove any leftover data files.
    cleanup_branch $1
    cleanup_branch $2

    # Create a file in the older branch.
    create_file $2 test_import

    # Now import it into the newer branch and verify.
    import_file $1 $2 test_import
    verify_file $1 test_import

    # Now downgrade by running wt from the older branch and dumping the table contents.
    #
    # Before trying this, we must remove the base configuration. The wt tool produces this file
    # however MongoDB will not so we should emulate this.
    rm $1/build/WT_TEST/WiredTiger.basecfg
    $2/build/wt -h $1/build/WT_TEST/ dump file:test_import
}

# The following associative array maps the 'official' build system to use for each branch.
# CMake build support is reliably mature in newer release branches, whilst earlier revisions
# primarily use Autoconf (note: some earlier branches may have CMake support, but these aren't
# considered 'mature' versions.)
declare -A build_sys
build_sys['develop']="cmake"
build_sys['mongodb-5.0']="autoconf"
build_sys['mongodb-4.4']="autoconf"
build_sys['mongodb-4.2']="autoconf"
build_sys['mongodb-4.0']="autoconf"
build_sys['mongodb-3.6']="autoconf"

# Release branches.
#
# Go all the way back to mongodb-4.2 since that's the first release where we don't support live
# import.
release_branches=(develop mongodb-5.0 mongodb-4.4 mongodb-4.2)

# Build each of the release branches.
for b in ${release_branches[@]}; do
    build_branch $b
done

for i in ${!release_branches[@]}; do
    newer=${release_branches[$i]}

    # MongoDB v4.2 doesn't support live import so it should only ever be used as the "older" branch
    # that we're importing from.
    if [ $newer = mongodb-4.2 ]; then
        continue
    fi

    older=${release_branches[$i+1]}
    import_compatibility_test $newer $older
done
