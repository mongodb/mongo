#!/bin/sh
# Run in the Docker daemon's mount namespace. $1 is the WSL drive mount prefix;
# the remaining arguments are lower-case Windows drive letters.
prefix=$1
shift
for drive in "$@"; do
    mountpoint="$prefix/$drive"
    uppercase_drive="$(printf '%s' "$drive" | tr '[:lower:]' '[:upper:]')"
    mkdir -p "$mountpoint"
    mountpoint -q "$mountpoint" || mount -t drvfs "$uppercase_drive:" "$mountpoint" ||
        echo "mount of $uppercase_drive: at $mountpoint failed"
done
grep -E ' /mnt/[a-z]+ ' /proc/mounts
ls -la /mnt
