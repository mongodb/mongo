#!/usr/bin/env bash
##############################################################################################
# Run WiredTiger test/format check against below 3 releases to ensure backward compatibility:
#   - current release (develop branch)
#   - previous release (WTx.y.z tag)
#   - previous previous release (WTa.b.c tag)
##############################################################################################

set -e

BUILD_DIR="build_posix"    # The relative directory of WiredTiger repo tree

###########################################################################
# This function will 
#		- retrieve the previous release tag number based on the count provided
#
#		arg1: count of previous release
###########################################################################
get_release()
{ 
	prev_cnt=$1
	rel=$(git tag | grep -v release | egrep "^[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,2}" | tail -$prev_cnt | head -1)
	echo "$rel"
}

#############################################################
# This function will 
#		- checkout git tree of the desired release (via arg1)
#		- make a  build
#		- use the generated binary 't' to test the configuration
#
#		arg1: release indicator (mapped to release branch/tag)
#############################################################
build_test_db()
{
	rel_ind="$1"
	echo "Starting to build and test format for \"$rel_ind\" branch/release ..."

	# Parse the release indicator into release number.
	# Checkout the release, and in the case of a branch, refresh it.
	case "$rel_ind" in
		"develop")  # current release
			rel="develop"
            git checkout $rel
            git pull --rebase
			;;
		"r1")       # previous release
			rel="$(get_release 1)"
            git checkout $rel
			;;
		"r2")       # previous previous release
			rel="$(get_release 2)"
            git checkout $rel
			;;
		*)
			echo "Unexpected branch/release number: \"$rel_ind\""
            exit 1
			;;
	esac

	echo "Building release: \"$rel\""

	# Ensure read only testing hasn't left things difficult to cleanup.
	chmod -R u+w ${BUILD_DIR}

	# Configure and build
	cd ${BUILD_DIR}
	sh reconf
	../configure --disable-strict --disable-shared --enable-diagnostic
	make -j $(grep -c ^processor /proc/cpuinfo)

	# Test the configuration
	cd test/format
	cat > CONFIG <<- EOF
	reverse=0
	runs=1
	rows=10000
	ops=1000
	threads=2
	evict_max=0
	compression=none
	logging_compression=none
	encryption=none
	EOF
	./t
	cd ../..
    
    	rm -f db

	# Go back to WiredTiger repo directory
	cd .. 

	# Archive the whole build_posix directory
	echo "Archiving ${BUILD_DIR} directory to ${BUILD_DIR}.${rel_ind} ..."
	rm -rf ${BUILD_DIR}.${rel_ind}	# if dir exists, the next cp command would write it as a sub-dir
	cp -R ${BUILD_DIR} ${BUILD_DIR}.${rel_ind}
}

#############################################################
# This function will
#   - go into the archived build directory for the release
#   - use the 'wt' binary to cross-check the wt URI
#
#   arg1: release indicator (mapped to release branch/tag)
#############################################################
verify_uri()
{
	rel_ind="$1"
	echo "Starting to verify URI for \"$rel_ind\" branch/release ..."

	# Go into the archived build directory of the release
	cd ${BUILD_DIR}.${rel_ind}

	dir=test/format/RUNDIR

	# Along the way, we changed the running configuration from
	# RUNDIR/run to RUNDIR/CONFIG.  Check for both.  Once that
	# change propogates back, we can just use $dir/CONFIG.
	if test -e $dir/CONFIG; then
	    cfile=$dir/CONFIG
	else
	    cfile=$dir/run
	fi
	isfile=`grep data_source $cfile | grep -c file || exit 0`
	if test "$isfile" -ne 0; then
	    uri="file:wt"
	else
	    uri="table:wt"
	fi

	# Cross-check the wt URI against the build directory
	case "$rel_ind" in
		"develop")  # Test directory of current release using wt binaries from "all 3 releases"
            echo "Verifying $uri using $devwt binary"
            # Remove basecfg as the statistic logging block
			rm -f $dir/WiredTiger.basecfg
            $devwt -h $dir verify $uri
			# We don't guarantee that the wiredtiger_open config is backwards
			# and forwards compatible.  Just delete it for now.
			echo "Verifying $uri using $r1wt binary"
			$r1wt -h $dir verify $uri
			echo "Verifying $uri using $r2wt binary"
			$r2wt -h $dir verify $uri
			;;
		"r1")      # Test directory of previous release using wt binaries from both "current" and "previous" releases
			rm -f $dir/WiredTiger.basecfg
            echo "Verifying $uri using $r1wt binary"
			$r1wt -h $dir verify $uri
			echo "Verifying $uri using $devwt binary"
			$devwt -h $dir verify $uri
			;;
		"r2")      # Test directory of previous previous release using wt binaries from both "current" and "previous previous" releases
			rm -f $dir/WiredTiger.basecfg
            echo "Verifying $uri using $r2wt binary"
			$r2wt -h $dir verify $uri
			echo "Verifying $uri using $devwt binary"
			$devwt -h $dir verify $uri
			;;
		*)
			echo "Unexpected argument value" 
			;;
	esac

	# Go back to WiredTiger repo directory
	cd .. 
}

# Firstly, do some cleanup for the previous run.
git reset --hard && git clean -fdqx -e '*.tgz'

# Build and test format, then set the archived wt binary for each release
build_test_db "develop"
devwt=../${BUILD_DIR}.develop/wt
build_test_db "r1"
r1wt=../${BUILD_DIR}.r1/wt
build_test_db "r2"
r2wt=../${BUILD_DIR}.r2/wt

# Cross-check the URI for the 3 releases 
verify_uri "develop"
verify_uri "r1"
verify_uri "r2"

