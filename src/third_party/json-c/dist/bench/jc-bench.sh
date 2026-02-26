#!/bin/sh
#
# Benchmarking harness for json-c
#
# Use this to compare the behavior of two different versions of the library,
# such as json-c-0.14 release vs master, master vs local changes, etc...
#

set -e

trap 'echo "FAILED!"' EXIT

RUNDIR=$(dirname "$0")
RUNDIR=$(cd "$RUNDIR" && pwd)

TOP=$(cd "$RUNDIR/.." && pwd)

usage()
{
	exitval=$1
	errmsg=$2
	if [ $exitval -ne 0 ] ; then
		exec 1>&2
	fi
	if [ ! -z "$errmsg" ] ; then
		echo "ERROR: $errmsg" 1>&2
	fi
	cat <<EOF
Usage: $0 [-h] [-v] [--build] [--run] [--compare] ...XAX...
EOF

	exit $extival
}

before_arg=
after_arg=
do_all=1
do_build=0
do_run=0
do_compare=0

while [ $# -gt 0 ] ; do
	case "$1" in
	--before)
		before_arg=$2
		shift
		;;
	--after)
		after_arg=$2
		shift
		;;
	--build)
		do_all=0
		do_build=1
		;;
	--run)
		do_all=0
		do_run=1
		;;
	--compare)
		do_all=0
		do_compare=1
		;;
	-h)
		usage 0 ""
		;;
	-v)
		set -x
		;;
	*)
		usage 1 "Unknown args: $*"
		;;
	esac
	shift
done

WORK="${RUNDIR}/work"
mkdir -p "${WORK}"

DATA="${RUNDIR}/data"
mkdir -p "${DATA}"

for file in citm_catalog.json twitter.json canada.json ; do
	if [ ! -r "${DATA}/${file}" ] ; then
		echo "Fetching ${file} from github.com/mloskot/json_benchmark"
		URL="https://github.com/mloskot/json_benchmark/raw/master/data/${file}"
		curl -s -L -o "${DATA}/${file}" "$URL"
	fi
done
echo

# Identify "after" commit hash, in order of preference
if [ ! -z "$after_arg" -a -d "$after_arg" ] ; then
	# Use provided directory
	after_src_dir="$after_arg"
	after_commit=
	echo "Using provided directory [$after_arg] as 'after'"
else
	_commit=
	if [ ! -z "$after_arg" ] ; then
		# Use provided commit hash
		_commit=$(git rev-parse --verify "$after_arg")
	fi
	if [ ! -z "$_commit" ] ;then
		after_src_dir=  # i.e. current tree
		after_commit="$_commit"
		echo "Using provided commit [$after_arg => $_commit] as 'after'"
	else
		# Local changes in current working directory
		# ${cur_branch}
		after_src_dir=$TOP
		after_commit=
		echo "Using local changes in $TOP as 'after'"
	fi
fi

# Identify "before" commit hash, in order of preference
if [ ! -z "$before_arg" -a -d "$before_arg" ] ; then
   	# Use provided directory
	before_src_dir="$before_arg"
	before_commit=
	echo "Using provided directory [$before_arg] as 'before'"
else
	_commit=
	if [ ! -z "$before_arg" ] ; then
		# Use provided commit hash
		_commit=$(git rev-parse --verify "$before_arg")
	fi
	if [ ! -z "$_commit" ] ;then
		before_src_dir=  # i.e. current tree
		before_commit="$_commit"
		echo "Using provided commit [$before_arg => $_commit] as 'before'"
	else
		# Use origin/${cur_branch}, if different from ${after_commit}
		_cur_branch=$(git rev-parse --abbrev-ref HEAD)
		_commit=
		if [ ! -z "${_cur_branch}" ] ; then
			_commit=$(git rev-parse --verify "origin/${_cur_branch}")
			echo "Using origin/${_cur_branch} [$_commit] as 'before'"
		fi
		if [ "$_commit" = "${after_commit}" ] ; then
			_commit=
		fi
	fi

	if [ ! -z "$_commit" ] ; then
		before_src_dir=  # i.e. current tree
		before_commit="$_commit"
	else
		# Use previous release
		before_src_dir=  # i.e. current tree
		before_commit="$(git tag | sort | tail -1)"
		echo "Using previous release [$before_commit] as 'before'"
	fi
fi

echo

compile_benchmark()
{
	local bname=$1
	local src_dir="$2"
	local src_commit="$3"

	local build_dir="${WORK}/$bname/build"
	local inst_dir="${WORK}/$bname/install"
	local bench_dir="${WORK}/$bname/bench"

	echo
	echo "=========== $bname ==========="
	echo

	mkdir -p "${build_dir}"
	mkdir -p "${inst_dir}"
	mkdir -p "${bench_dir}"

	if [ ! -z "$src_commit" ] ; then
		# Resolve the short hash, tag or branch name to full hash
		src_commit=$(git rev-parse $src_commit)
	fi

	# No src dir specified, clone and checkout $src_commit
	if [ -z "$src_dir" ] ; then
		src_dir="${WORK}/$bname/src"
		echo "=== Using sources in $src_dir"
		mkdir -p "$src_dir"
		at_commit=$(git --git-dir="$src_dir/.git" rev-parse HEAD 2> /dev/null || true)
		echo "at_commit: $at_commit"
		if [ -z "$at_commit" ] ; then
			# Assume it's an empty dir
			git clone -n "$TOP" "$src_dir"
		fi
		git -C "$src_dir" --git-dir="$src_dir/.git" checkout "$src_commit"
	fi
	# else, use the provided $src_dir

	if [ -e "${src_dir}/CMakeLists.txt" ] ; then
		cd "${build_dir}"
		cmake -DCMAKE_INSTALL_PREFIX="${inst_dir}" "${src_dir}"
	else
		# Old versions of json-c used automake/autoconf
		cd "${src_dir}"
		sh autogen.sh   # always run it, configure doesn't always work
		cd "${build_dir}"
		"${src_dir}/configure" --prefix="${inst_dir}"
	fi
	make all install

	cd "${bench_dir}"
	cmake -DCMAKE_PREFIX_PATH="${inst_dir}" "${TOP}/apps"
	make all
}

# XXX TODO: name "after" and "before" uniquely using the dir & commit

if [ $do_all -ne 0 -o $do_build -ne 0 ] ; then
	sleep 5   # Wait slightly, to allow the human to read the message
	          #  about what exactly we're doing to benchmark.
	compile_benchmark "after" "${after_src_dir}" "${after_commit}"
	compile_benchmark "before" "${before_src_dir}" "${before_commit}"
fi

run_benchmark()
{
	local bname=$1
	local inst_dir="${WORK}/$bname/install"
	local bench_dir="${WORK}/$bname/bench"

	local INPUT=${DATA}/canada.json

	cd "${bench_dir}"
	mkdir -p results
	(time ./json_parse -n "${INPUT}") > results/basic_timing.out 2>&1
	valgrind --tool=massif --massif-out-file=massif.out ./json_parse -n "${INPUT}"
	ms_print massif.out > results/ms_print.out
	heaptrack -o heaptrack_out ./json_parse -n "${INPUT}"
	heaptrack --analyze heaptrack_out.gz -H histogram.out > results/heaptrack.out
	awk ' { s=$1; count=$2; ru=(int((s+ 15) / 16)) * 16; wasted = ((ru-s)*count); print s, count, ru-s, wasted; total=total+wasted} END { print "Total: ", total }' histogram.out > results/histogram2.out

	# XXX stamp some info about what was built & run into ./results/.

	echo "DONE with $bname"
}

if [ $do_all -ne 0 -o $do_run -ne 0 ] ; then
	run_benchmark "after"
	run_benchmark "before"
fi

if [ $do_compare -ne 0 ] ; then
	# XXX this needs better analysis
	cd "${WORK}"
	diff -udr before/bench/results after/bench/results || true
else
	echo "To compare results, run:"
	echo "$0 --compare"
fi

trap '' EXIT

:<<=cut

Benchmarks to run:

* Parse JSON strings, of various sizes and characteristics
    * Flags: STRICT vs. non-STRICT, validate UTF8

* Serialization time
    * plain, spaces, pretty

* json_c_visit tests
* JSON pointer tests

Things to record and compare:

* Running time
* Peak memory usage
* Useful bytes vs. overhead for memory allocations
* Total number of allocations
* Average allocation size
* Log of all allocation sizes

=cut

