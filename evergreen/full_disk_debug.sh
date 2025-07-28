# What percent a filesystem has to be filled to be considered "full"
FULL_DISK_THRESHOLD=90
HAS_FULL_DISK=false
declare -a FULL_DISKS

# This will output the percent each filesystem is full in the following format:
# 91% /dev/root /
# 7% /dev/nvme0n1p15 /System/Volumes/Data
# 34% /dev/nvme1n1 /System/Volumes/Update
FILESYSTEMS=$(df -H | grep -vE '^Filesystem|tmpfs|cdrom|map' | awk '{ print $5 " " $1 " " $9 }')
while read -r output; do
    usep=$(echo "$output" | awk '{ print $1}' | cut -d'%' -f1)
    partition=$(echo "$output" | awk '{ print $2 }')
    mountpoint=$(echo "$output" | awk '{ print $3 }')
    if [ $usep -ge $FULL_DISK_THRESHOLD ]; then
        echo "Running out of space \"$partition ($usep%)\" on $(hostname) as on $(date)"
        HAS_FULL_DISK=true
        FULL_DISKS+=("$mountpoint")
    fi
done <<<"$FILESYSTEMS"

if $HAS_FULL_DISK; then
    echo "Checking disks"
    for item in "${FULL_DISKS[@]}"; do
        # print all files that are above one megabyte sorted
        du -cha "$item" 2>/dev/null | grep -E "^[0-9]+(\.[0-9]+)?[G|M|T]" | sort -h
    done
else
    echo "No full partitions found, skipping"
fi
