#! /bin/sh

trap 'rm -f __*; exit 1' 1 2

build_top=../..
bdb=$build_top/db/build_unix

colflag=0
dump_bdb=0
while :
	do case "$1" in
	# -b means we need to dump the BDB database
	-b)
		dump_bdb=1;
		shift ;;
	# -c means it was a column-store.
	-c)
		colflag=1
		shift ;;
	*)
		break ;;
	esac
done

if test $# -ne 0; then
	echo 'usage: s_dumpcmp [-bc]' >&2
	exit 1
fi

ext='"../../ext/collators/reverse/.libs/reverse_collator.so"'
bzext="../../ext/compressors/bzip2_compress/.libs/bzip2_compress.so"
if test -e $bzext ; then
        ext="$ext,\"$bzext\""
fi
config='extensions=['$ext']'

$build_top/wt -C "$config" dump file:__wt | sed -e '1,/^Data$/d' > __wt_dump

if test $dump_bdb -ne 1; then
	exit 0
fi

if test $colflag -eq 0; then
	$bdb/db_dump -p __bdb |
	    sed -e '1,/HEADER=END/d' \
		-e '/DATA=END/d' \
		-e 's/^ //' > __bdb_dump
else
	# Format stores record numbers in Berkeley DB as string keys,
	# it's simpler that way.  Convert record numbers from strings
	# to numbers.
	$bdb/db_dump -p __bdb |
	    sed -e '1,/HEADER=END/d' \
		-e '/DATA=END/d' \
		-e 's/^ //' |
	    sed -e 's/^0*//' \
		-e 's/\.00$//' \
		-e N > __bdb_dump
fi

cmp __wt_dump __bdb_dump > /dev/null

exit $?
