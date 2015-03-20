#! /bin/sh

trap 'exit 1' 1 2

top=../..

home="RUNDIR"
wturi="file:wt"

colflag=0
bdbdir=""
while :
	do case "$1" in
	# -b means we need to dump the Berkeley DB database
	-b)
		bdbdir="$2";
		shift ; shift ;;
	# -c means it was a column-store.
	-c)
		colflag=1
		shift ;;
	-h)
		shift ;
		home=$1
		shift;;
	-n)
		shift ;
		wturi=$1
		shift ;;
	*)
		break ;;
	esac
done

if test $# -ne 0; then
	echo 'usage: s_dumpcmp [-bc]' >&2
	exit 1
fi

$top/wt -h $home dump $wturi |
    sed -e '1,/^Data$/d' > $home/wt_dump

if test "X$bdbdir" = "X"; then
	exit 0
fi

if test $colflag -eq 0; then
	$bdbdir/bin/db_dump -p $home/bdb |
	    sed -e '1,/HEADER=END/d' \
		-e '/DATA=END/d' \
		-e 's/^ //' > $home/bdb_dump
else
	# Format stores record numbers in Berkeley DB as string keys,
	# it's simpler that way.  Convert record numbers from strings
	# to numbers.
	$bdbdir/bin/db_dump -p $home/bdb |
	    sed -e '1,/HEADER=END/d' \
		-e '/DATA=END/d' \
		-e 's/^ //' |
	    sed -e 's/^0*//' \
		-e 's/\.00$//' \
		-e N > $home/bdb_dump
fi

cmp $home/wt_dump $home/bdb_dump > /dev/null

exit $?
