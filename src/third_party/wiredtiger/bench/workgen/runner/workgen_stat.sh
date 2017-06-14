#!/bin/bash
#
# workgen_stat.sh - combine JSON time series output from WT and workgen.
#
Usage() {
    cat <<EOF
Usage: $0 [ options ]
Options:
    -h <WT_home_directory>     # set the WiredTiger home directory
    -e <analyzer_name>         # run analyzer on the combined files
    -o <output_file>           # output file for result 

At least one of '-t2' or '-o' must be selected.
EOF
    exit 1
}

Filter() {
    sed -e 's/"version" *: *"[^"]*",//' "$@"
}

wthome=.
outfile=
analyze=

while [ "$#" != 0 ]; do
    arg="$1"
    shift
    case "$arg" in
        -h )
            if [ $# = 0 ]; then
                Usage
            fi
            wthome="$1"
            shift
            ;;
        -o )
            if [ $# = 0 ]; then
                Usage
            fi
            outfile="$1"
            shift
            ;;
        -e )
            if [ $# = 0 ]; then
                Usage
            fi
            analyze="$1"
            shift
            ;;
    esac
done
if [ ! -d "$wthome" ]; then
    echo "$wthome: WT home directory does not exist"
    exit 1
fi
if [ ! -f "$wthome/WiredTiger.wt" ]; then
    echo "$wthome: directory is not a WiredTiger home directory"
    exit 1
fi
if [ "$outfile" = '' ]; then
   if [ "$analyze" = false ]; then
       Usage
   fi
   outfile="$wthome/stat_tmp.json"
fi
(cd $wthome; Filter WiredTigerStat.* sample.json) | sort > $outfile
if [ "$analyze" != '' ]; then
    sysname=`uname -s`
    if [ "$sysname" = Darwin ]; then
        open -a "$analyze" "$outfile"
    else
        "$analyze" "$outfile"
    fi
fi
