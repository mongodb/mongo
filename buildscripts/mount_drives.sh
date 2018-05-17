#!/bin/bash
# Script used to mount /data & /log on a separate drive from root.
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
  -l <deviceName>,     OPTIONAL, Device name to mount /log on, i.e., "xvdd".
                       For Windows, specify the drive letter, i.e., "e".
  -t <fsType>,         File system type, defaults to '$fs_type'.
                       Not supported on Windows.
  -o <mountOptions>,   File system mount options, i.e., "-m crc=0,finobt=0".
                       Not supported on Windows.
  -u <user:group>,     User:Group to make owner of /data & /log. Defaults to '$user_group'.
EOF
}


# Parse command line options
while getopts "d:l:o:r:t:u:?" option
do
  case $option in
    d)
      data_device_names=$OPTARG
      ;;
    l)
      log_device_name=$OPTARG
      ;;
    o)
      mount_options=$OPTARG
      ;;
    r)
      data_raid_device_name=$OPTARG
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

function mount_drive {
  local root_dir=$1
  local sub_dirs=$2
  local device_names=$3
  local raid_device_name=$4
  local mount_options=$5
  local fs_type=$6
  local user_group=$7

  # Determine how many devices were specified
  local num_devices=0
  for device_name in $device_names
  do
    local devices="$devices /dev/$device_name"
    let num_devices=num_devices+1
  done

  # $OS is defined in Cygwin
  if [ "Windows_NT" = "$OS" ]; then
    if [ $num_devices -ne 1 ]; then
      echo "Must specify only one drive"
      _usage_
      exit 1
    fi

    local drive_poll_retry=0
    local drive_poll_delay=0
    local drive_retry_max=40

    local drive=$device_names
    local system_drive=c

    while true;
    do
      sleep $drive_poll_delay
      echo "Looking for drive '$drive' to mount $root_dir"
      if [ -d /cygdrive/$drive ]; then
        echo "Found drive"
        rm -rf /$root_dir
        rm -rf /cygdrive/$system_drive/$root_dir
        mkdir $drive:\\$root_dir
        cmd.exe /c mklink /J $system_drive:\\$root_dir $drive:\\$root_dir
        ln -s /cygdrive/$drive/$root_dir /$root_dir
        setfacl -s user::rwx,group::rwx,other::rwx /cygdrive/$drive/$root_dir
        for sub_dir in $sub_dirs
        do
            mkdir -p /cygdrive/$drive/$root_dir/$sub_dir
        done
        chown -R $user_group /cygdrive/$system_drive/$root_dir
        break
      fi
      let drive_poll_retry=drive_poll_retry+1
      if [ $drive_poll_retry -eq $drive_retry_max ]; then
        echo "Timed out trying to mount $root_dir drive."
        exit 1
      fi
      let drive_poll_delay=drive_poll_delay+5
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

    # Mount the $root_dir drive(s)
    /sbin/mkfs.$fs_type $mount_options -f $device_name
    echo "$device_name /$root_dir auto noatime 0 0" | tee -a /etc/fstab
    mkdir /$root_dir || true
    chmod 777 /$root_dir
    mount -t $fs_type $device_name /$root_dir
    for sub_dir in $sub_dirs
    do
      mkdir -p /$root_dir/$sub_dir
      chmod 1777 /$root_dir/$sub_dir
    done
    chown -R $user_group /$root_dir
  else
    echo "Unsupported OS '$(uname)'"
    exit 0
  fi
}

mount_drive data "db tmp" "$data_device_names" "$data_raid_device_name" "$mount_options" "$fs_type" "$user_group"
mount_drive log "" "$log_device_name" "" "$mount_options" "$fs_type" "$user_group"
