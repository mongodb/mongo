#!/bin/bash

# wtperf_xray.sh - run wtperf regression tests with xray profiling and generate
# profiling information.
#
# This script assumes it is running in the directory with the wtperf executable.
#
# Usage
# wtperf_xray.sh <wtperf-config-file> [-h output-directory] [wtperf other args]
#
# This script checks the first argument after the wtperf configuration to see
# whether a home directory is being specified with the -h flag. If so, this
# script will write its output files to that directory. Otherwise it will
# default to WT_TEST (wtperf's default).
#
# Environment variables
# XRAY_BINARY --
#	The binary to use to inspect the xray log. (default: llvm-xray)
# FLAME_GRAPH_PATH --
#	The path to your copy of Brendan Gregg's FlameGraph script. (optional)
#
# When this is complete you can find information in the following files:
# wtperf_account.txt --
#	The top 10 functions where the workload is spending the most time along
#	with a count, min, max and some percentiles for each one.
# wtperf_stacks.txt --
#	The top 10 stack traces where the workload is spending the most time.
#	This calculation is done separately per thread.
# wtperf_graph.svg --
#	A function call graph showing what functions call each other. The edges
#	are labelled and coloured proportionally to represent the ratio of time
#	spent in each function call.
# wtperf_flame.svg --
#	A graph visualising stack traces and the time spent within each stack
#	frame. If FLAME_GRAPH_PATH is not specified, this graph won't be
#	generated.
#
if ! test -f ./wtperf; then
	echo "$0: could not find wtperf in current working directory"
	exit 1
fi

if test "$#" -lt "1"; then
	echo "$0: must specify wtperf configuration to run"
	exit 1
fi

# By default, wtperf uses WT_TEST as its home directory.
xray_home="WT_TEST"
if test "$2" = "-h"; then
	if ! test -z "$3"; then
		xray_home="$3"
	fi
fi
echo "$0: using $xray_home as home directory"

# Check symbols to ensure we've compiled with XRay.
objdump -h -j xray_instr_map ./wtperf > /dev/null
if test "$?" -ne "0"; then
	echo "$0: wtperf not compiled with xray, add '-fxray-instrument' to your CFLAGS"
	exit 1
fi

if ! test -d "$xray_home"; then
	echo "$0: creating directory $xray_home"
	mkdir "$xray_home"
fi

xray_account_path="${xray_home}/wtperf_account.txt"
xray_stack_path="${xray_home}/wtperf_stack.txt"
xray_graph_path="${xray_home}/wtperf_graph.svg"
xray_flame_path="${xray_home}/wtperf_flame.svg"

rm xray-log.wtperf.* \
	"$xray_account_path" \
	"$xray_stack_path" \
	"$xray_graph_path" \
	"$xray_flame_path" &> /dev/null

export XRAY_OPTIONS="patch_premain=true xray_mode=xray-basic verbosity=1"
./wtperf -O "$@"

xray_log=$(ls xray-log.wtperf.*)
num_logs=$(echo "$xray_log" | wc -w)
if test "$num_logs" -ne "1"; then
	echo "$0: detected more than one xray log"
	exit 1
fi

if test -z "$XRAY_BINARY"; then
	xray_bin="llvm-xray"
	echo "$0: XRAY_BINARY is unset, defaulting to $xray_bin"
else
	xray_bin="$XRAY_BINARY"
fi

$xray_bin account "$xray_log" \
	-top=10 -sort=sum -sortorder=dsc -instr_map ./wtperf > \
	"$xray_account_path"

# Use the -per-thread-stacks option to get the top 10 stacks for each thread.
# We could use the -aggregate-threads flag here so get the top stacks for all threads (omitting duplicates).
$xray_bin stack -per-thread-stacks "$xray_log" \
	-instr_map ./wtperf > \
	"$xray_stack_path"

# Generate a DOT graph.
$xray_bin graph "$xray_log" \
	-m ./wtperf -color-edges=sum -edge-label=sum | \
	unflatten -f -l10 | \
	dot -Tsvg -o "$xray_graph_path"

# This file can be inspected in the Google Chrome Trace Viewer.
# It seems to take a long time to generate this so just disable it for now.
# $xray_bin convert -symbolize -instr_map=./wtperf -output-format=trace_event $xray_log | gzip > wtperf_trace.txt.gz
if test -z "$FLAME_GRAPH_PATH"; then
	echo "$0: FLAME_GRAPH_PATH is unset, skipping flame graph generation"
else
	$xray_bin stack "$xray_log" \
		-instr_map ./wtperf -stack-format=flame -aggregation-type=time -all-stacks | \
		"$FLAME_GRAPH_PATH" > "$xray_flame_path"
fi
