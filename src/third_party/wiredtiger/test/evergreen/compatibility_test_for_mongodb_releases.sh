#!/usr/bin/env bash
##############################################################################################
# Check releases to ensure backward compatibility.
##############################################################################################

set -e

#############################################################
# build_release:
#       arg1: release
#############################################################
build_release()
{
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Building release: \"$1\""
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

        git clone --quiet https://github.com/wiredtiger/wiredtiger.git "$1"
        cd "$1"
        git checkout --quiet "$1"

        config=""
        config+="--enable-diagnostic "
        config+="--enable-snappy "
        (sh build_posix/reconf &&
            ./configure $config && make -j $(grep -c ^processor /proc/cpuinfo)) > /dev/null
}

#############################################################
# run_format:
#       arg1: release
#       arg2: access methods list
#############################################################
run_format()
{
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Running format in release: \"$1\""
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

        cd "$1/test/format"

        args=""
        args+="cache=80 "                       # Medium cache so there's eviction
        args+="checkpoints=1 "                  # Force periodic writes
        args+="compression=snappy "             # We only built with snappy, force the choice
        args+="data_source=table "
        args+="in_memory=0 "                    # Interested in the on-disk format
        args+="leak_memory=1 "                  # Faster runs
        args+="logging_compression=snappy "     # We only built with snappy, force the choice
        args+="rebalance=0 "                    # Faster runs
        args+="rows=1000000 "
        args+="salvage=0 "                      # Faster runs
        args+="timer=4 "
        args+="verify=0 "                       # Faster runs

        for am in $2; do
            dir="RUNDIR.$am"
            echo "./t running $am access method..."
            ./t -1q -h $dir "file_type=$am" $args
        done
}

EXT="extensions=["
EXT+="ext/compressors/snappy/.libs/libwiredtiger_snappy.so,"
EXT+="ext/collators/reverse/.libs/libwiredtiger_reverse_collator.so, "
EXT+="ext/encryptors/rotn/.libs/libwiredtiger_rotn.so, "
EXT+="]"

#############################################################
# verify_backward:
#       arg1: release #1
#       arg2: release #2
#       arg3: access methods list
#############################################################
verify_backward()
{
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Release \"$1\" verifying \"$2\""
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        
        cd "$1"
        for am in $3; do
            dir="$2/test/format/RUNDIR.$am"
            echo "$1/wt verifying $2 access method $am..."

            WIREDTIGER_CONFIG="$EXT" ./wt -h "../$dir" verify table:wt
        done
}

# Create a directory in which to do the work.
top="test-compatibility-run"
rm -rf "$top" && mkdir "$top"
cd "$top"

# Build the releases.
(build_release mongodb-3.4)
(build_release mongodb-3.6)
(build_release mongodb-4.0)
(build_release mongodb-4.2)
#(build_release mongodb-4.4)
(build_release "develop")

# Run format in each release for supported access methods.
(run_format mongodb-3.4 "fix row var")
(run_format mongodb-3.6 "fix row var")
(run_format mongodb-4.0 "fix row var")
(run_format mongodb-4.2 "fix row var")
#(run_format mongodb-4.4 "row")
(run_format "develop" "row")

# Verify backward compatibility for supported access methods.
(verify_backward mongodb-3.6 mongodb-3.4 "fix row var")
(verify_backward mongodb-4.0 mongodb-3.6 "fix row var")
(verify_backward mongodb-4.2 mongodb-4.0 "fix row var")
#(verify_backward mongodb-4.4 mongodb-4.2 "fix row var")
#(verify_backward develop mongodb-4.4 "row")
 (verify_backward develop mongodb-4.2 "fix row var")

exit 0
