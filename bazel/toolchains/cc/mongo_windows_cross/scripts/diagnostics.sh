#!/bin/sh
# $1: action working directory, $2: executable. Use the container utilities' PATH.
PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
echo "working directory: $1"
ls -ld "$1" 2>&1
echo "tool: $2"
ls -lL "$2" 2>&1
readlink -f "$2" 2>&1
echo "drive mounts inside the container:"
grep -E ' /mnt/[a-z]+(/| )' /proc/mounts 2>&1
ls -la /mnt 2>&1
