# What percent a filesystem has to be filled to be considered "full"
FULL_DISK_THRESHOLD=90
HAS_FULL_DISK=false

# This will output the percent each filesystem is full in the following format:
# 91% /dev/root
# 7% /dev/nvme0n1p15
# 34% /dev/nvme1n1
FILESYSTEMS=$(df -H | grep -vE '^Filesystem|tmpfs|cdrom' | awk '{ print $5 " " $1 }')
while read -r output; do
    usep=$(echo "$output" | awk '{ print $1}' | cut -d'%' -f1)
    partition=$(echo "$output" | awk '{ print $2 }')
    if [ $usep -ge $FULL_DISK_THRESHOLD ]; then
        echo "Running out of space \"$partition ($usep%)\" on $(hostname) as on $(date)"
        HAS_FULL_DISK=true
    fi
done <<<"$FILESYSTEMS"

if $HAS_FULL_DISK; then
    # print all files that are above one megabyte sorted
    du -cha / 2>/dev/null | grep -E "^[0-9]+(\.[0-9]+)?[G|M|T]" | sort -h
else
    echo "No full partitions found, skipping"
fi
