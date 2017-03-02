#!/bin/bash
set -e

if ! [ -a bongotop ]
then
    echo "need a bongotop binary in the same directory as the smoke script"
    exit 1
fi

chmod 755 bongotop

./bongotop > output.out &
bongotop_pid=$!

sleep 5

kill $bongotop_pid

headers=( "ns" "total" "read" "write" )
for header in "${headers[@]}"
do 
    if [ `head -2 output.out | grep -c $header` -ne 1 ]
    then
        echo "header row doesn't contain $header"
        exit 1
    fi
done

if [ `head -5 output.out | grep -c ms` -ne 3 ]
then
    echo "subsequent lines don't contain ms totals"
    exit 1
fi 
