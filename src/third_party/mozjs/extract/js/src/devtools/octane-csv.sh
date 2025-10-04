#!/usr/bin/env bash

set -e -o pipefail

function echo_to_stderr {
    echo "$1" 1>&2
}

function usage_and_exit {
    echo_to_stderr "Usage:"
    echo_to_stderr "    $0 <path-to-js> <number-of-iterations>"
    echo_to_stderr
    echo_to_stderr "Run octane <number-of-iterations> times, and aggregate the results"
    echo_to_stderr "into one CSV file, which is written to stdout."
    echo_to_stderr
    echo_to_stderr "See the js/src/devtools/plot-octane.R script for plotting the"
    echo_to_stderr "results."
    echo_to_stderr
    echo_to_stderr "Complete example usage with plotting:"
    echo_to_stderr
    echo_to_stderr "    \$ ./js/src/devtools/octane-csv.sh path/to/js 20 > control.csv"
    echo_to_stderr
    echo_to_stderr "    Next, apply some patch you'd like to test."
    echo_to_stderr
    echo_to_stderr "    \$ ./js/src/devtools/octane-csv.sh path/to/js 20 > variable.csv"
    echo_to_stderr "    \$ ./js/src/devtools/plot-octane.R control.csv variable.csv"
    echo_to_stderr
    echo_to_stderr "    Open Rplots.pdf to view the results."
    exit 1
}

if [[ "$#" != "2" ]]; then
    usage_and_exit
fi

# Get the absolute, normalized $JS path, and ensure its an executable.

JS_DIR=$(dirname $1)
if [[ ! -d "$JS_DIR" ]]; then
    echo_to_stderr "error: no such directory $JS_DIR"
    echo_to_stderr
    usage_and_exit
fi

JS=$(basename $1)
cd "$JS_DIR" > /dev/null
JS="$(pwd)/$JS"
if [[ ! -e "$JS" ]]; then
    echo_to_stderr "error: '$JS' is not executable"
    echo_to_stderr
    usage_and_exit
fi
cd - > /dev/null

# Go to the js/src/octane directory.

cd $(dirname $0)/../octane > /dev/null

# Run octane and transform the results into CSV.
#
# Run once as a warm up, and to grab the column headers. Then run the benchmark
# $ITERS times, grabbing just the data rows.

echo_to_stderr "Warm up"
"$JS" ./run.js | grep -v -- "----" | cut -f 1 -d ':' | tr '\n' ','
echo

ITERS=$2
while [[ "$ITERS" -ge "1" ]]; do
    echo_to_stderr "Iterations left: $ITERS"
    "$JS" ./run.js | grep -v -- "----" | cut -f 2 -d ':' | tr '\n' ','
    echo
    ITERS=$((ITERS - 1))
done

echo_to_stderr "All done :)"
