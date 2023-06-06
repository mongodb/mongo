#!/bin/sh

set -e

usage()
{
	printf "USAGE:\n\t$0 [-eq|-ne|-lt|-le|-gt|-ge] FILE1 FILE2\n"
}

help()
{
	printf "Small utility to compare file sizes without printing them with set -x.\n\n"
	usage
}

case "$1" in
	-h) help; exit 0  ;;
	--help) help; exit 0 ;;
esac

if ! test -f $2; then
	printf "FILE1='%b' is not a file\n\n" "$2"
	usage
	exit 1
fi

if ! test -f $3; then
	printf "FILE2='%b' is not a file\n\n" "$3"
	usage
	exit 1
fi


size1=$(wc -c < $2)
size2=$(wc -c < $3)

case "$1" in
	-eq) [ "$size1" -eq "$size2" ] ;;
	-ne) [ "$size1" -ne "$size2" ] ;;
	-lt) [ "$size1" -lt "$size2" ] ;;
	-le) [ "$size1" -le "$size2" ] ;;
	-gt) [ "$size1" -gt "$size2" ] ;;
	-ge) [ "$size1" -ge "$size2" ] ;;
esac
