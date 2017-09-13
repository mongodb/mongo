#!/bin/bash
# Script used to mount /data on a separate drive from root.
# This script must be invoked either by a root user or with sudo.
# Currently only supports Windows & Linux.
# See _usage_ for how this script should be invoked.

set -o errexit

# Default options
fs_type=xfs
user_group=$USER:$(id -Gn $USER | cut -f1 -d ' ')

# _usage_: Provides usage infomation
function _usage_ {
  cat << EOF
usage: $0 options
This script supports the following parameters for Windows & Linux platforms:
  -d <deviceNames>,    REQUIRED, Space separated list of devices to mount /data on,
                       i.e., "xvdb xvdc", more than one device indicates a RAID set.
                       For Windows, specify the drive letter, i.e., "d".
  -r <raidDeviceName>, REQUIRED, if more the one device is specified in <deviceNames>,
                       i.e., md0.
                       Not supported on Windows.
  -t <fsType>,         File system type, defaults to '$fs_type'.
                       Not supported on Windows.
  -o <mountOptions>,   File system mount options, i.e., "-m crc=0,finobt=0".
                       Not supported on Windows.
  -u <user:group>,     User:Group to make owner of /data. Defaults to '$user_group'.
EOF
}


# Parse command line options
while getopts "d:o:r:t:u:?" option
do
   case $option in
     d)
        device_names=$OPTARG
        ;;
     o)
        mount_options=$OPTARG
        ;;
     r)
        raid_device_name=$OPTARG
        ;;
     t)
        fs_type=$OPTARG
        ;;
     u)
        user_group=$OPTARG
        ;;
     \?|*)
        _usage_
        exit 0
        ;;
    esac
done

# Determine how many devices were specified
num_devices=0
for device_name in $device_names
do
  devices="$devices /dev/$device_name"
  let "num_devices=num_devices+1"
done

# $OS is defined in Cygwin
if [ "Windows_NT" = "$OS" ]; then
    if [ $num_devices -ne 1 ]; then
        echo "Must specify only one drive"
        _usage_
        exit 1
    fi

    i=0
    DRIVE_POLL_DELAY=1
    DRIVE_RETRY_MAX=240

    drive=$device_names
    system_drive=c

    while true; do
        sleep "$DRIVE_POLL_DELAY"
        echo "Looking for drive '$drive' to mount /data"
        if [ -d "/cygdrive/$drive" ]; then
            echo "Found drive"
            rm -rf /data
            rm -rf /cygdrive/$system_drive/data
            mkdir -p /cygdrive/$drive/data/db
            cmd.exe /c mklink /J $system_drive:\\data $drive:\\data
            ln -s /cygdrive/$system_drive/data /data
            chown -R $user_group /data
            break
        fi
        let "i=i+1"
        if [ "$i" -eq "$DRIVE_RETRY_MAX" ]; then
            echo "TIMED OUT trying to mount /data drive."
            exit 1
        fi
    done

elif [ $(uname | awk '{print tolower($0)}') = "linux" ]; then
    if [ $num_devices -eq 0 ]; then
        echo "Must specify atleast one device"
        _usage_
        exit 1
    elif [ $num_devices -gt 1 ]; then
        if [ -z "$raid_device_name" ]; then
            echo "Missing RAID device name"
            _usage_
            exit 1
        fi
    fi

    # Unmount the current devices, if already mounted
    umount /mnt || true
    umount $devices || true

    # Determine if we have a RAID set
    if [ ! -z "$raid_device_name" ]; then
        echo "Creating RAID set on '$raid_device_name' for devices '$devices'"
        device_name=/dev/$raid_device_name
        /sbin/udevadm control --stop-exec-queue
        yes | /sbin/mdadm --create $device_name --level=0 -c256 --raid-devices=$num_devices $devices
        /sbin/udevadm control --start-exec-queue
        /sbin/mdadm --detail --scan > /etc/mdadm.conf
        /sbin/blockdev --setra 32 $device_name
    else
        device_name=$devices
    fi

    # Mount the /data drive(s)
    /sbin/mkfs.$fs_type $mount_options -f $device_name
    echo "$device_name /data auto noatime 0 0" | tee -a /etc/fstab
    mount -t $fs_type $device_name /data
    mkdir -p /data/db || true
    chown -R $user_group /data
    mkdir /data/tmp
    chmod 1777 /data/tmp
else
    echo "Unsupported OS '$(uname)'"
    exit 0
fi
