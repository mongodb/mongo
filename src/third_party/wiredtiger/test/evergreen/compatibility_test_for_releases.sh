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
# pick_a_version:
#       arg1: branch name
#############################################################
pick_a_version()
{
    branch=$1

    # Query out all released patch versions for a given release branch using "git tag"
    local versions=()

    # Avoid picking below types of versions:
    #   - release candidates (rc)
    #   - alpha releases (alpha)
    #   - mongodb-4.4.0 through mongodb-4.4.6 (4.4.[0-6]$) as they are not compatible with Doxygen
    #     version 1.8.17 (installed on the build hosts). WT-7437 was introduced since 4.4.7.
    mapfile -t versions < <( git tag | grep $branch | grep -Ev "rc|alpha|4.4.[0-6]$" )

    # Randomly pick a version from the array of patch versions
    pv=${versions[$RANDOM % ${#versions[@]} ]}
    echo "$pv"
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


    # Check if the branch directory exists, if not then clone
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
        (mkdir -p build && cd build &&
            $CMAKE $config ../. && make -j $(grep -c ^processor /proc/cpuinfo)) > /dev/null
    else
        config+="--enable-snappy "
        config+="--disable-standalone-build "

        # Check if the build directory already exists, if so recompile the code without remaking the directory
        (mkdir -p build && cd build && sh ../build_posix/reconf &&
            ../configure $config && make -j $(grep -c ^processor /proc/cpuinfo)) > /dev/null
        # Copy out the extension modules to their parent directory. This is done to maintain uniformity between
        # autoconf and CMake build directories, where relative module paths can possibly be cached when running verify/upgrade_downgrade
        # tests between branch directories i.e. in the connection configuration.
        cp build/ext/compressors/snappy/.libs/libwiredtiger_snappy.so build/ext/compressors/snappy/libwiredtiger_snappy.so
        cp build/ext/collators/reverse/.libs/libwiredtiger_reverse_collator.so build/ext/collators/reverse/libwiredtiger_reverse_collator.so
        cp build/ext/encryptors/rotn/.libs/libwiredtiger_rotn.so build/ext/encryptors/rotn/libwiredtiger_rotn.so
    fi

}

#############################################################
# get_config_file_name:
#       arg1: branch name
#############################################################
get_config_file_name()
{
    local file_name=""
    branch_name=$1
    format_dir="$branch_name/build/test/format"
    if [ "${wt_standalone}" = true ] || [ $older = true ] ; then
        file_name="${format_dir}/CONFIG_default"
        echo $file_name
        return
    fi
    file_name="CONFIG_${branch_name}"

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
        echo " WARNING - ${file_name} already exists, overwriting it."
    fi

    echo "##################################################" > $file_name
    echo "runs.type=row" >> $file_name              # WT-7379 - Temporarily disable column store tests
    echo "btree.prefix=0" >> $file_name             # WT-7579 - Prefix testing isn't portable between releases
    echo "cache=80" >> $file_name                   # Medium cache so there's eviction
    echo "checksum=on" >> $file_name                # WT-7851 Fix illegal checksum configuration
    echo "checkpoints=1"  >> $file_name             # Force periodic writes
    echo "compression=snappy"  >> $file_name        # We only built with snappy, force the choice
    echo "data_source=table" >> $file_name
    echo "huffman_key=0" >> $file_name              # WT-6893 - Not supported by newer releases
    echo "in_memory=0" >> $file_name                # Interested in the on-disk format
    echo "leak_memory=1" >> $file_name              # Faster runs
    echo "logging=1" >> $file_name                  # Test log compatibility
    echo "logging_compression=snappy" >> $file_name # We only built with snappy, force the choice
    echo "rows=1000000" >> $file_name
    echo "salvage=0" >> $file_name                  # Faster runs
    echo "timer=4" >> $file_name
    echo "verify=1" >> $file_name                   # Faster runs

    # Append older release configs for newer compatibility release test
    if [ $newer = true ]; then
        for i in "${compatible_upgrade_downgrade_release_branches[@]}"
        do
            if [ "$i" == "$branch_name" ] ; then
                echo "transaction.isolation=snapshot" >> $file_name # WT-7545 - Older releases can't do lower isolation levels
                echo "transaction.timestamps=1" >> $file_name       # WT-7545 - Older releases can't do non-timestamp transactions
                break
            fi
        done
    fi
    echo "##################################################" >> $file_name
}

#############################################################
# create_default_configs:
# This function will create the default configs for older and standalone
# release branches.
#############################################################
create_default_configs()
{
    # Iterate over the release branches and create configuration files
    for b in `ls`; do
        if [ -d "$b" ]; then
            (create_configs $b)
            if [ -f CONFIG_$b ]; then
                cp -rf CONFIG_$b $b/test/format/
            fi
        fi
    done
}

#############################################################
# create_configs_for_newer_release_branches:
#############################################################
create_configs_for_newer_release_branches()
{
    # Create configs for all the newer releases
    for b in ${newer_release_branches[@]}; do
        (create_configs $b)
    done

    # Copy per-release configs in the newer release branches
    for b in ${newer_release_branches[@]}; do
        format_dir="$b/build/test/format"
        cp -rf CONFIG* $format_dir
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

    format_dir="$branch_name/build/test/format"
    cd "$format_dir"
    flags="-1q $(bflag $branch_name)"

    config_file=""

    # Compatibility test for newer releases will have CONFIG file for each release
    # branches for the upgrade/downgrade testing.
    #
    # Compatibility test for older and standalone releases will have the default config.
    if [ "${wt_standalone}" = true ] || [ $older = true ]; then
        config_file="-c CONFIG_default"
    else
        config_file="-c CONFIG_${branch_name}"
    fi

    for am in $2; do
        dir="RUNDIR.$am"
        echo "./t running $am access method..."
        ./t $flags ${config_file} -h $dir "file_type=$am"

        # Remove the version string from the base configuration file. (MongoDB does not create
        # a base configuration file, but format does, so we need to remove its version string
        # to allow backward compatibility testing.)
        (echo '/^version=/d'
            echo w) | ed -s $dir/WiredTiger.basecfg > /dev/null
    done
    cd -
}

#############################################################
# is_test_checkpoint_recovery_supported:
#       arg1: branch name
#############################################################
is_test_checkpoint_recovery_supported()
{
    branch_name=$1

    #
    # The test_checkpoint recovery change WT-7958 are included in mongo
    # 4.4.9 and 5.0.3 (and newer) versions.
    #
    # There's no much value to run test_checkpoint for earlier versions.
    #
    # Exclude mongodb-4.4.10+ and mongodb-5.0.10+ versions.
    #
    # Exclude mongodb-4.4 and mongodb-5.0 and they represent latest code
    # of the corresponding release branches, which have WT-7958 included.
    #
    if ( ([[ $branch_name == mongodb-* ]] &&
          [[ $branch_name < "mongodb-4.4.9" ]]) ||
         ([[ $branch_name == mongodb-5.0.[0-9] ]] &&
          [[ $branch_name < "mongodb-5.0.3" ]]) ) &&
       [[ $branch_name != mongodb-4.4.[1-9][0-9] ]] &&
       [[ $branch_name != mongodb-5.0.[1-9][0-9] ]] &&
       [[ $branch_name != "mongodb-4.4" ]] &&
       [[ $branch_name != "mongodb-5.0" ]]
    then
        echo "no"
    else
        echo "yes"
    fi
}

#############################################################
# run_test_checkpoint:
#       arg1: branch name
#       arg2: access methods list
#############################################################
run_test_checkpoint()
{
    branch_name=$1

    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Running test checkpoint in branch: \"$branch_name\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    cd "$branch_name/build/test/checkpoint"

    if [ "${build_sys[$branch_name]}" == "cmake" ]; then
        test_bin="test_checkpoint"
    else
        test_bin="t"
    fi

    # With the timestamp and prepare transactions configuration, this test
    # can produce a scenario where the on-disk tables have more data than
    # the checkpoint can see.
    #
    # During the verification stage, rollback to stable has to be performed
    # with the checkpoint snapshot to achieve the consistency.
    flags="-W 3 -D -x -n 100000 -k 100000 -C cache_size=100MB"

    for am in $2; do
        dir="RUNDIR.$am"
        echo "./t running $am access method..."
        if [ "$am" == "fix" ]; then
            ./$test_bin f $flags -h $dir
        elif [ "$am" == "var" ]; then
            ./$test_bin -t c $flags -h $dir
        else
            ./$test_bin -t r $flags -h $dir
        fi
    done
    cd -
}

#############################################################
# run_tests:
#       arg1: branch name
#       arg2: access methods list
#############################################################
run_tests()
{
    run_format $1 $2
    run_test_checkpoint $1 $2
}

EXT="extensions=["
EXT+="build/ext/compressors/snappy/libwiredtiger_snappy.so,"
EXT+="build/ext/collators/reverse/libwiredtiger_reverse_collator.so, "
EXT+="build/ext/encryptors/rotn/libwiredtiger_rotn.so, "
EXT+="]"

#############################################################
# verify_test_format:
#       arg1: branch name #1
#       arg2: branch name #2
#       arg3: access methods list
#       arg4: backward compatibility
#############################################################
verify_test_format()
{
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Release \"$1\" format verifying \"$2\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    cd "$1"

    wt_bin="build/wt"
    for am in $3; do
        echo "$1/$wt_bin verifying $2 access method $am..."
        dir="$2/build/test/format/RUNDIR.$am"
        WIREDTIGER_CONFIG="$EXT" ./$wt_bin $(bflag $1) -h "../$dir" verify table:wt

        if [ "$4" = true ]; then
            echo "$1/wt dump and load $2 access method $am..."
            WIREDTIGER_CONFIG="$EXT" ./$wt_bin $(bflag $1) -h "../$dir" dump table:wt > dump_wt.txt
            WIREDTIGER_CONFIG="$EXT" ./$wt_bin $(bflag $1) -h "../$dir" load -f dump_wt.txt
        fi
    done

    cd -
}

#############################################################
# verify_test_checkpoint:
#       arg1: branch name #1
#       arg2: branch name #2
#       arg3: access methods list
#############################################################
verify_test_checkpoint()
{
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Version \"$1\" test_checkpoint verifying \"$2\""
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    top_dir=$PWD
    cd "$1/build/test/checkpoint"

    if [ "${build_sys[$1]}" == "cmake" ]; then
        test_bin="test_checkpoint"
    else
        test_bin="t"
    fi

    for am in $3; do
        echo "$1/$test_bin verifying $2 access method $am..."
        dir="$top_dir/$2/build/test/checkpoint/RUNDIR.$am"
        cp -fr "$dir" "$dir.backup"
        if [ "$am" = "fix" ]; then
            ./$test_bin -t f -D -v -h "$dir"
        elif [ "$am" = "var" ]; then
            ./$test_bin -t c -D -v -h "$dir"
        else
            ./$test_bin -t r -D -v -h "$dir"
        fi
    done

    cd -
}

#############################################################
# verify_branches:
#       arg1: branch name #1
#       arg2: branch name #2
#       arg3: access methods list
#       arg4: backward compatibility
#############################################################
verify_branches()
{
    verify_test_format $1 $2 $3 $4
    verify_test_checkpoint $1 $2 $3
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

    cfg_file_branch1=$(get_config_file_name $1)
    cfg_file_branch2=$(get_config_file_name $2)

    format_dir_branch1="$1/build/test/format"
    format_dir_branch2="$2/build/test/format"

    # Alternate running each branch format test program on the second branch's build.
    # Loop twice, that is, run format twice using each branch.
    top="$PWD"
    for am in $3; do
        for reps in {1..2}; do
            echo "$1 format running on $2 access method $am..."
            cd "$top/$format_dir_branch1"
            flags="-1Rq $(bflag $1)"
            ./t $flags -c "$top/$format_dir_branch2/${cfg_file_branch1}" -h "$top/$format_dir_branch2/RUNDIR.$am" timer=2

            echo "$2 format running on $2 access method $am..."
            cd "$top/$format_dir_branch2"
            flags="-1Rq $(bflag $2)"
            ./t $flags -c $cfg_file_branch2 -h "RUNDIR.$am" timer=2
        done
    done
}

#############################################################
# test_upgrade_to_branch:
#       arg1: release branch name
#       arg2: path to test data folder
#############################################################
test_upgrade_to_branch()
{
    cd $1/build/test/checkpoint

    if [ "${build_sys[$1]}" == "cmake" ]; then
        test_bin="test_checkpoint"
    else
        test_bin="t"
    fi

    for FILE in $2/*; do
        # Run actual test.
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "Upgrading $FILE database to $1..."

        # Disable exit on non 0
        set +e

        output="$(./$test_bin -t r -D -v -h $FILE)"
        test_res=$?

        # Enable exit on non 0
        set -e

        # Validate test result.
        if [[ "$FILE" =~ "4.4."[0-6]"_unclean"$ ]]; then
            echo "Databases generated with unclean shutdown from versions 4.4.[0-6] must fail."
            if [[ "$test_res" == 0 ]]; then
                echo "$output"
                echo "Error: Upgrade of $FILE database to $1 has not failed!"
                exit 1
            fi
        elif [[ "$test_res" != 0 ]]; then
            echo "$output"
            echo "Error: Upgrade of $FILE database to $1 failed! Test result is $test_res."
            exit 1
        fi

        echo "Success!"
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    done
}

#############################################################
# prepare_test_data_wt_8395:
#############################################################
prepare_test_data_wt_8395()
{
    echo "Preparing test data..."
    git clone --quiet --depth 1 --filter=blob:none --no-checkout https://github.com/wiredtiger/mongo-tests.git
    cd mongo-tests
    git checkout --quiet master -- WT-8395 &> /dev/null
    cd WT-8395

    for FILE in *; do tar -zxf $FILE; done
    rm *.tar.gz; cd ../..
}

# Only one of below flags will be set by the 1st argument of the script.
older=false
newer=false
wt_standalone=false
patch_version=false
upgrade_to_latest=false

# Branches in below 2 arrays should be put in newer-to-older order.
#
# An overlap (last element of the 1st array & first element of the 2nd array)
# is expected to avoid missing the edge testing coverage.
#
# The 2 arrays should be adjusted over time when newer branches are created,
# or older branches are EOL.
newer_release_branches=(develop mongodb-5.0 mongodb-4.4 mongodb-4.2)
older_release_branches=(mongodb-4.2 mongodb-4.0 mongodb-3.6)

# This array is used to generate compatible configuration files between releases, because
# upgrade/downgrade test runs each build's format test program on the second build's
# configuration file.
compatible_upgrade_downgrade_release_branches=(mongodb-4.4 mongodb-4.2)

# This array is used to configure the release branches we'd like to run patch version
# upgrade/downgrade test.
patch_version_upgrade_downgrade_release_branches=(mongodb-5.0 mongodb-4.4)

# This array is used to configure the release branches we'd like to run test checkpoint
# upgrade/downgrade test.
test_checkpoint_release_branches=(develop mongodb-5.0 mongodb-4.4)

# This array is used to configure the release branches we'd like to run upgrade to latest test.
upgrade_to_latest_upgrade_downgrade_release_branches=(mongodb-5.0 mongodb-4.4)

declare -A scopes
scopes[newer]="newer stable release branches"
scopes[older]="older stable release branches"
scopes[patch_version]="patch versions of the same release branch"
scopes[upgrade_to_latest]="upgrade/downgrade databases to the latest versions of the codebase"
scopes[wt_standalone]="WiredTiger standalone releases"
scopes[two_versions]="any two given versions"

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

#############################################################
# usage string
#############################################################
usage()
{
    echo -e "Usage: \tcompatibility_test_for_releases [-n|-o|-p|-u|-w|-v]"
    echo -e "\t-n\trun compatibility tests for ${scopes[newer]}"
    echo -e "\t-o\trun compatibility tests for ${scopes[older]}"
    echo -e "\t-p\trun compatibility tests for ${scopes[patch_version]}"
    echo -e "\t-u\trun compatibility tests for ${scopes[upgrade_to_latest]}"
    echo -e "\t-w\trun compatibility tests for ${scopes[wt_standalone]}"
    echo -e "\t-v <v1> <v2>\trun compatibility tests for ${scopes[two_versions]}"
    exit 1
}

if [ $# -lt 1 ]; then
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
"-p")
    patch_version=true
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Performing compatibility tests for ${scopes[patch_version]}"
;;
"-u")
    upgrade_to_latest=true
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Performing compatibility tests for ${scopes[upgrade_to_latest]}"
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
;;
"-w")
    wt_standalone=true
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Performing compatibility tests for ${scopes[wt_standalone]}"
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
;;
"-v")
    two_versions=true
    v1=$2
    v2=$3
    [[ -z "$v1" || -z "$v2" ]] && usage
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "Performing compatibility tests for $v1 and $v2"
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

if [ "$upgrade_to_latest" = true ]; then
    test_root=$(pwd)
    test_data_root="$test_root/mongo-tests"
    test_data="$test_root/mongo-tests/WT-8395"

    for b in ${upgrade_to_latest_upgrade_downgrade_release_branches[@]}; do
        # prepare test data and test upgrade to the branch b.
        (prepare_test_data_wt_8395) && \
        (build_branch $b) && \
        (test_upgrade_to_branch $b $test_data)

        # cleanup.
        cd $test_root
        rm -rf $test_data_root
    done
fi

if [ "$two_versions" = true ]; then
    # Check if the 2 given versions support test checkpoint with recovery
    rtn=$(is_test_checkpoint_recovery_supported $v1)
    if [ $rtn == "no" ]; then
        echo -e "\n\"$v1\" does not support test checkpoint with recovery, exiting ...\n"
        exit 1
    fi

    rtn=$(is_test_checkpoint_recovery_supported $v2)
    if [ $rtn == "no" ]; then
        echo -e "\n\"$v2\" does not support test checkpoint with recovery, exiting ...\n"
        exit 1
    fi

    # Build the branches
    (build_branch $v1)
    (build_branch $v2)

    # Run test for both branches to generate data files
    (run_test_checkpoint $v1 "row")
    (run_test_checkpoint $v2 "row")

    # Use one version binary to verify data files generated by the other version
    (verify_test_checkpoint "$v1" "$v2" "row")
    (verify_test_checkpoint "$v2" "$v1" "row")

    exit 0
fi

if [ "$patch_version" = true ]; then
    for b in ${patch_version_upgrade_downgrade_release_branches[@]}; do
        # Build the tip of the release branch and run test to generate data files
        (build_branch $b)
        (run_test_checkpoint "$b" "row")

        # Pick a patch version from the list of patch versions for the release branch
        cd $b; pv=$(pick_a_version $b); cd ..

        (build_branch $pv)
        rtn=$(is_test_checkpoint_recovery_supported $pv)
        patch_fix_included=$(git log --oneline --grep=WT-8708 -b "$pv" --)
 
        # Only run verify if the picked version supports test checkpoint recovery
        if [ $rtn == "no" ]; then
            echo -e "\n\"$pv\" does not support test checkpoint with recovery, skipping ...\n"
        # Apply patch fix from WT-8708 to already released compatible versions to avoid test/checkpoint setting commit timestamp less than stable timestamp
        elif [ $rtn == "yes" ] && [ ! $patch_fix_included ]; then
            cd $pv;

            git format-patch -1 d4b0ad6cacb874fdc20bcc76311d789dd5a01441;
            patch -p1 < 0001-WT-8708-Fix-timestamp-usage-error-in-test-checkpoint.patch;

            cd ..;
            (build_branch $pv)

            (run_test_checkpoint "$pv" "row")
            # Use one version binary to verify data files generated by the other version
            (verify_test_checkpoint "$b" "$pv" "row")
            (verify_test_checkpoint "$pv" "$b" "row")
        fi
    done

    exit 0
fi

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

if [ "$newer" = true ]; then
    create_configs_for_newer_release_branches
else
    create_default_configs
fi

# Run format in each branch for supported access methods.
if [ "$newer" = true ]; then
    for b in ${newer_release_branches[@]}; do
        (run_format $b "row")
    done
    for b in ${test_checkpoint_release_branches[@]}; do
        (run_test_checkpoint $b "row")
    done
fi

if [ "$older" = true ]; then
    for b in ${older_release_branches[@]}; do
        (run_format $b "fix row var")
    done
fi

if [ "${wt_standalone}" = true ]; then
    (run_tests "$wt1" "row")
    (run_format "$wt2" "row")
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
        (verify_test_format ${newer_release_branches[$i]} ${newer_release_branches[$((i+1))]} "row" true)
    done
    for i in ${!test_checkpoint_release_branches[@]}; do
        [[ $((i+1)) < ${#test_checkpoint_release_branches[@]} ]] && \
        (verify_test_checkpoint ${test_checkpoint_release_branches[$i]} ${test_checkpoint_release_branches[$((i+1))]} "row")
    done
fi

if [ "$older" = true ]; then
    for i in ${!older_release_branches[@]}; do
        [[ $((i+1)) < ${#older_release_branches[@]} ]] && \
        (verify_test_format ${older_release_branches[$i]} ${older_release_branches[$((i+1))]} "fix row var" true)
    done
fi

if [ "${wt_standalone}" = true ]; then
    (verify_branches develop "$wt1" "row" true)
    (verify_test_format "$wt1" "$wt2" "row" true)
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
        (verify_test_format ${newer_release_branches[$((i+1))]} ${newer_release_branches[$i]} "row" false)
    done
    for i in ${!test_checkpoint_release_branches[@]}; do
        [[ $((i+1)) < ${#test_checkpoint_release_branches[@]} ]] && \
        (verify_test_checkpoint ${test_checkpoint_release_branches[$((i+1))]} ${test_checkpoint_release_branches[$i]} "row")
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
