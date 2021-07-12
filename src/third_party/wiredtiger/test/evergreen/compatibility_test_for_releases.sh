#!/usr/bin/env bash
##############################################################################################
# Check branches to ensure forward/backward compatibility, including some upgrade/downgrade testing.
##############################################################################################

set -e

#############################################################
# bflag:
#       arg1: branch name
#############################################################
bflag()
{
        # Return if the branch's format command takes the -B flag for backward compatibility.
        test "$1" = "develop" && echo "-B "
        test "$1" = "mongodb-5.0" && echo "-B "
        test "$1" = "mongodb-4.4" && echo "-B "
        return 0
}

#############################################################
# get_prev_version:
#       arg1: branch name
#############################################################
get_prev_version()
{
    # Sort the list of WiredTiger tags numerically, then pick out the argument number of releases
    # from the end of the list. That is, get a list of releases in numeric order, then pick out
    # the last release (argument "1"), the next-to-last release (argument "2") and so on. Assumes
    # WiredTiger releases are tagged with just numbers and decimal points.
    echo "$(git tag | egrep '^[0-9][0-9.]*$' | sort -g | tail -$1 | head -1)"
}

#############################################################
# build_branch:
#       arg1: branch name
#############################################################
build_branch()
{
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Building branch: \"$1\""
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

        git clone --quiet https://github.com/wiredtiger/wiredtiger.git "$1"
        cd "$1"
        git checkout --quiet "$1"

        config=""
        config+="--enable-snappy "
        config+="--disable-standalone-build "
        (sh build_posix/reconf &&
            ./configure $config && make -j $(grep -c ^processor /proc/cpuinfo)) > /dev/null
}

#############################################################
# get_config_file_name:
#       arg1: branch name
#############################################################
get_config_file_name()
{
    branch_name=$1
    release=$(echo $branch_name |  cut -c9-)

    local file_name="CONFIG_${release}"

    # Special case to generate develop branch CONFIG
    if [ -z "$release" ] && [ ${branch_name} == "develop" ] ; then
        release="develop"
        file_name="CONFIG_${release}"
    fi
    echo $file_name
}

#############################################################
# create_configs:
#       arg1: branch name
#############################################################
create_configs()
{
    branch_name=$1

    file_name=$(get_config_file_name $branch_name)

    if [ -f $file_name ] ; then
        return
    fi

cat > $file_name << EOF
runs.type=row               # Temporarily disable column store tests
btree.prefix=0              # Prefix testing isn't portable between releases
cache=80                    # Medium cache so there's eviction
checkpoints=1               # Force periodic writes
compression=snappy          # We only built with snappy, force the choice
data_source=table
huffman_key=0               # Not supoprted by newer releases
in_memory=0                 # Interested in the on-disk format
leak_memory=1               # Faster runs
logging=1                   # Test log compatibility
logging_compression=snappy  # We only built with snappy, force the choice
rows=1000000
salvage=0                   # Faster runs
timer=4
verify=0                    # Faster runs
EOF

    # Append older release configs
    for i in "${older_release_branches[@]}"
    do
        if [ "$i" == "$branch_name" ] ; then
            echo "transaction.isolation=snapshot    # Older releases can't do lower isolation levels" >> $file_name
            echo "transaction.timestamps=1          # Older releases can't do non-timestamp transactions" >> $file_name
            break
        fi
    done
}

#############################################################
# create_configs_per_release:
#############################################################
create_configs_per_release()
{
    # Create configs for all the newer releases
    for b in ${newer_release_branches[@]}; do
        (create_configs $b)
    done

    # Copy per-release configs in the newer release branches
    for b in ${newer_release_branches[@]}; do
        cp -rf CONFIG* $b/test/format/
    done

    # Delete configs from the top folder
    rm -rf CONFIG*
}

#############################################################
# run_format:
#       arg1: branch name
#       arg2: access methods list
#############################################################
run_format()
{
        branch_name=$1
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Running format in branch: \"$branch_name\""
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

        cd "$branch_name/test/format"
        release=$(echo $branch_name |  cut -c9-)

        flags="-1q $(bflag $branch_name)"
        if [ -z "$release" ] && [ ${branch_name} == "develop" ] ; then
            release="develop"
        fi

        for am in $2; do
            dir="RUNDIR.$am"
            echo "./t running $am access method..."
            ./t $flags -c "CONFIG_${release}" -h $dir "file_type=$am"

            # Remove the version string from the base configuration file. (MongoDB does not create
            # a base configuration file, but format does, so we need to remove its version string
            # to allow backward compatibility testing.)
            (echo '/^version=/d'
             echo w) | ed -s $dir/WiredTiger.basecfg > /dev/null
        done
}

EXT="extensions=["
EXT+="ext/compressors/snappy/.libs/libwiredtiger_snappy.so,"
EXT+="ext/collators/reverse/.libs/libwiredtiger_reverse_collator.so, "
EXT+="ext/encryptors/rotn/.libs/libwiredtiger_rotn.so, "
EXT+="]"

#############################################################
# verify_branches:
#       arg1: branch name #1
#       arg2: branch name #2
#       arg3: access methods list
#       arg4: backward compatibility
#############################################################
verify_branches()
{
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Release \"$1\" verifying \"$2\""
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

        cd "$1"
        for am in $3; do
            echo "$1/wt verifying $2 access method $am..."
            dir="$2/test/format/RUNDIR.$am"
            WIREDTIGER_CONFIG="$EXT" ./wt $(bflag $1) -h "../$dir" verify table:wt

            if [ "$4" = true ]; then
                echo "$1/wt dump and load $2 access method $am..."
                WIREDTIGER_CONFIG="$EXT" ./wt $(bflag $1) -h "../$dir" dump table:wt > dump_wt.txt
                WIREDTIGER_CONFIG="$EXT" ./wt $(bflag $1) -h "../$dir" load -f dump_wt.txt
            fi
        done
}

#############################################################
# upgrade_downgrade:
#       arg1: branch name #1
#       arg2: branch name #2
#       arg3: access methods list
#############################################################
upgrade_downgrade()
{
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Upgrade/downgrade testing with \"$1\" and \"$2\""
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

        file_name_1=$(get_config_file_name $1)
        file_name_2=$(get_config_file_name $2)

        # Alternate running each branch format test program on the second branch's build.
        # Loop twice, that is, run format twice using each branch.
        top="$PWD"
        for am in $3; do
            for reps in {1..2}; do
                echo "$1 format running on $2 access method $am..."
                cd "$top/$1/test/format"
                flags="-1Rq $(bflag $1)"
                ./t $flags -c "$top/$2/test/format/${file_name_1}" -h "$top/$2/test/format/RUNDIR.$am" timer=2

                echo "$2 format running on $2 access method $am..."
                cd "$top/$2/test/format"
                flags="-1Rq $(bflag $2)"
                ./t $flags -c $file_name_2 -h "RUNDIR.$am" timer=2
            done
        done
}

# Only one of below flags will be set by the 1st argument of the script.
older=false
newer=false
wt_standalone=false

# Branches in below 2 arrays should be put in newer-to-older order.
#
# An overlap (last element of the 1st array & first element of the 2nd array)
# is expected to avoid missing the edge testing coverage.
#
# The 2 arrays should be adjusted over time when newer branches are created,
# or older branches are EOL.
newer_release_branches=(develop mongodb-5.0 mongodb-4.4 mongodb-4.2)
older_release_branches=(mongodb-4.2 mongodb-4.0 mongodb-3.6)

declare -A scopes
scopes[newer]="newer stable release branches"
scopes[older]="older stable release branches"
scopes[wt_standalone]="WiredTiger standalone releases"

#############################################################
# usage string
#############################################################
usage()
{
    echo -e "Usage: \tcompatibility_test_for_releases [-n|-o|-w]"
    echo -e "\t-n\trun compatibility tests for ${scopes[newer]}"
    echo -e "\t-o\trun compatibility tests for ${scopes[older]}"
    echo -e "\t-w\trun compatibility tests for ${scopes[wt_standalone]}"
    exit 1
}

if [ $# -ne 1 ]; then
    usage
fi

# Script argument processing
case $1 in
"-n")
    newer=true
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Performing compatibility tests for ${scopes[newer]}"
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
;;
"-o")
    older=true
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Performing compatibility tests for ${scopes[older]}"
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
;;
"-w")
    wt_standalone=true
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Performing compatibility tests for ${scopes[wt_standalone]}"
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
;;
*)
    usage
;;
esac

# Create a directory in which to do the work.
top="test-compatibility-run"
rm -rf "$top" && mkdir "$top"
cd "$top"

# Build the branches.
if [ "$newer" = true ]; then
    for b in ${newer_release_branches[@]}; do
        (build_branch $b)
    done
fi

if [ "$older" = true ]; then
    for b in ${older_release_branches[@]}; do
        (build_branch $b)
    done
fi

# Get the names of the last two WiredTiger releases, wt1 is the most recent release, wt2 is the
# release before that. Minor trickiness, we depend on the "develop" directory already existing
# so we have a source in which to do git commands.
if [ "${wt_standalone}" = true ]; then
    (build_branch develop)
    cd develop; wt1=$(get_prev_version 1); cd ..
    (build_branch "$wt1")
    cd develop; wt2=$(get_prev_version 2); cd ..
    (build_branch "$wt2")
fi

create_configs_per_release

# Run format in each branch for supported access methods.
if [ "$newer" = true ]; then
    for b in ${newer_release_branches[@]}; do
        (run_format $b "row")
    done
fi

if [ "$older" = true ]; then
    for b in ${older_release_branches[@]}; do
        (run_format $b "fix row var")
    done
fi

if [ "${wt_standalone}" = true ]; then
    (run_format "$wt1" "fix row var")
    (run_format "$wt2" "fix row var")
fi

# Verify backward compatibility for supported access methods.
#
# The branch array includes a list of branches in newer-to-older order.
# For backport compatibility, the binary of the newer branch should
# be used to verify the data files generated by the older branch.
# e.g. (verify_branches mongodb-4.4 mongodb-4.2 "row")
if [ "$newer" = true ]; then
    for i in ${!newer_release_branches[@]}; do
        [[ $((i+1)) < ${#newer_release_branches[@]} ]] && \
        (verify_branches ${newer_release_branches[$i]} ${newer_release_branches[$((i+1))]} "row" true)
    done
fi

if [ "$older" = true ]; then
    for i in ${!older_release_branches[@]}; do
        [[ $((i+1)) < ${#older_release_branches[@]} ]] && \
        (verify_branches ${older_release_branches[$i]} ${older_release_branches[$((i+1))]} "fix row var" true)
    done
fi

if [ "${wt_standalone}" = true ]; then
    (verify_branches develop "$wt1" "row" true)
    (verify_branches "$wt1" "$wt2" "row" true)
fi

# Verify forward compatibility for supported access methods.
#
# The branch array includes a list of branches in newer-to-older order.
# For forward compatibility, the binary of the older branch should
# be used to verify the data files generated by the newer branch.
# e.g. (verify_branches mongodb-4.2 mongodb-4.4 "row")
if [ "$newer" = true ]; then
    for i in ${!newer_release_branches[@]}; do
        [[ $((i+1)) < ${#newer_release_branches[@]} ]] && \
        (verify_branches ${newer_release_branches[$((i+1))]} ${newer_release_branches[$i]} "row" false)
    done
fi

# Upgrade/downgrade testing for supported access methods.
if [ "$newer" = true ]; then
    for i in ${!newer_release_branches[@]}; do
        [[ $((i+1)) < ${#newer_release_branches[@]} ]] && \
        (upgrade_downgrade ${newer_release_branches[$((i+1))]} ${newer_release_branches[$i]} "row")
    done
fi

exit 0
