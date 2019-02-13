#!/usr/bin/env bash
##############################################################################################
# Check releases to ensure forward and backward compatibility.
##############################################################################################

###########################################################################
# Return the most recent version of the tagged release.
###########################################################################
get_release()
{
	echo "$(git tag | grep "^mongodb-$1.[0-9]" | sort -V | sed -e '$p' -e d)"
}

#############################################################
# This function will
#	- checkout git tree of the desired release and build it,
#	- generate test objects.
#
# arg1: MongoDB tagged release number or develop branch identifier.
#############################################################
build_rel()
{
	echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
	echo "Building release: \"$1\""
	echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

	git clone --quiet https://github.com/wiredtiger/wiredtiger.git "wt.$1" > /dev/null || return 1
	cd "wt.$1" || return 1

	config=""
	config+="--enable-snappy "

	case "$1" in
        # Please note 'develop' here is planned as the future MongoDB release 4.2 - the only release that supports
        # both enabling and disabling of timestamps in data format. Once 4.2 is released, we need to update this script.
	"develop")
		branch="develop";;
	"develop-timestamps")
		branch="develop"
		config+="--enable-page-version-ts";;
	*)
		branch=$(get_release "$1");;
	esac

	git checkout --quiet -b $branch || return 1

	(sh build_posix/reconf && ./configure $config && make -j $(grep -c ^processor /proc/cpuinfo)) > /dev/null || return 1

	cd test/format || return 1

	# Run a configuration and generate some on-disk files.
	args=""
	args+="cache=80 "				# Medium cache so there's eviction
	args+="checkpoints=1 "				# Force periodic writes
	args+="compression=snappy "			# We only built with snappy, force the choice
	args+="data_source=table "
	args+="in_memory=0 "				# Interested in the on-disk format
	args+="leak_memory=1 "				# Faster runs
	args+="logging_compression=snappy "		# We only built with snappy, force the choice
	args+="quiet=1 "
	args+="rebalance=0 "				# Faster runs
	args+="rows=1000000 "
	args+="salvage=0 "				# Faster runs
	args+="timer=4 "
	args+="verify=0 "				# Faster runs
	for am in fix row var; do
		./t -h "RUNDIR.$am" -1 "file_type=$am" $args || return 1
	done

	return 0
}

#############################################################
# This function will
#	- verify a pair of releases can verify each other's objects.
#
# arg1: release #1
# arg2: release #2
#############################################################
verify()
{
	echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
	echo "Verifying release \"$1\" and \"$2\""
	echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
	a="wt.$1"
	b="wt.$2"

	EXT="extensions=["
	EXT+="ext/compressors/snappy/.libs/libwiredtiger_snappy.so,"
	EXT+="ext/collators/reverse/.libs/libwiredtiger_reverse_collator.so, "
	EXT+="ext/encryptors/rotn/.libs/libwiredtiger_rotn.so, "
	EXT+="]"
	
	cd $a || return 1
	for am in fix row var; do
		echo "$a/wt verifying $b/test/format/RUNDIR.$am..."
		WIREDTIGER_CONFIG="$EXT" \
		    ./wt -h ../$b/test/format/RUNDIR.$am verify table:wt || return 1
	done

	cd ../$b || return 1
	for am in fix row var; do
		echo "$b/wt verifying $a/test/format/RUNDIR.$am..."
		WIREDTIGER_CONFIG="$EXT" \
		    ./wt -h ../$a/test/format/RUNDIR.$am verify table:wt || return 1
	done

	return 0
}

run()
{
	# Build test files from each release.
	(build_rel 3.4) || return 1
	(build_rel 3.6) || return 1
	(build_rel 4.0) || return 1
	(build_rel develop) || return 1
	(build_rel develop-timestamps) || return 1

	# Verify forward/backward compatibility.
	(verify 3.4 3.6) || return 1
	(verify 3.6 4.0) || return 1
	(verify 4.0 develop) || return 1
	(verify 4.0 develop-timestamps) || return 1
	(verify develop develop-timestamps) || return 1

	return 0
}

# Create a directory in which to do the work.
top="test-compatibility-run"
rm -rf $top && mkdir $top && cd $top || {
	echo "$0: unable to create $top working directory"
	exit 1
}

run
exit $?
