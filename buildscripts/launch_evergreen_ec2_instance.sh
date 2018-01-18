#!/bin/bash
# Script used to clone an AWS EC2 instance in Evergreen from the current variant.
# Currently only supports Windows & Linux.
# See _usage_ for how this script should be invoked.

set -o errexit

# Default options
tag_name="Evergreen AMI"

# _usage_: Provides usage infomation
function _usage_ {
  cat << EOF
usage: $0 options
This script supports the following parameters for Windows & Linux platforms:
  -k <ssh_key_id>,     [REQUIRED] The ssh key id used to access the new AWS EC2 instance.
  -y <aws_ec2_yml>,    [REQUIRED] YAML file name where to store the new AWS EC2 instance
                       information. This file will be used in etc/evergreen.yml for
                       macro expansion of variables used in other functions.
  -e <expire_hours>,   [OPTIONAL] Number of hours to expire the AWS EC2 instance.
  -g <security_group_id>, [OPTIONAL] The security group id to be used for the new AWS EC2 instance.
                          To specify more than one group, invoke this option each time.
  -n <subnet_id>,      [OPTIONAL] The subnet id in which to launch the AWS EC2 instance.
  -s <security_group>, [OPTIONAL] The security group to be used for the new AWS EC2 instance.
                       To specify more than one group, invoke this option each time.
  -t <tag_name>,       [OPTIONAL] The tag name of the new AWS EC2 instance.
EOF
}

# Parse command line options
while getopts "e:g:k:n:s:t:y:?" option
do
   case $option in
     e)
        expire_hours=$OPTARG
        ;;
     g)
        sec_group_ids="$sec_group_ids $OPTARG"
        ;;
     k)
        ssh_key_id=$OPTARG
        ;;
     n)
        subnet_id=$OPTARG
        ;;
     s)
        sec_groups="$sec_groups $OPTARG"
        ;;
     t)
        tag_name=$OPTARG
        ;;
     y)
        aws_ec2_yml=$OPTARG
        ;;
     \?|*)
        _usage_
        exit 0
        ;;
    esac
done

if [ -z $aws_ec2_yml ]; then
  echo "Must specify aws_ec2_yml file"
  exit 1
fi

if [ -z $ssh_key_id ]; then
  echo "Must specify ssh_key_id"
  exit 1
fi

for sec_group in $sec_groups
do
  security_groups="$security_groups --securityGroup $sec_group"
done

for sec_group_id in $sec_group_ids
do
  security_group_ids="$security_group_ids --securityGroupIds $sec_group_id"
done

if [ ! -z $subnet_id ]; then
  subnet_id="--subnetId $subnet_id"
fi

if [ ! -z $expire_hours ]; then
  expire_tag="--tagExpireHours $expire_hours"
fi

# Get the AMI information on the current host so we can launch a similar EC2 instance.
# See http://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-instance-metadata.html#instancedata-data-retrieval
aws_metadata_url="http://169.254.169.254/latest/meta-data"
ami=$(curl -s $aws_metadata_url/ami-id)
instance_type=$(curl -s $aws_metadata_url/instance-type)
echo "AMI EC2 info: $ami $instance_type"

data_df=$(df -BG -T /data | tail -n1 | tr -s ' ')
base_device_name=$(echo $data_df | cut -f1 -d ' ' | sed -e 's/.*\///g')
fstype=$(echo $data_df | cut -f2 -d ' ')
device_size=$(echo $data_df | cut -f3 -d ' ' | cut -f1 -d "G" | cut -f1 -d ".")
if [[ -z "$base_device_name" || -z "$fstype" || -z "$device_size" ]]; then
  echo "Could not detect /data mount point, one of the following are not detected:"
  echo "base_device_name: '$base_device_name' fstype: '$fstype' device_size: '$device_size'"
  exit 1
fi

# Linux variants have block device mappings, so discover the mounted drives.
if [ $(uname) = "Linux" ]; then
  devices_info="$base_device_name;$fstype;$device_size"

  is_raid_device=$(grep -s active /proc/mdstat | grep $base_device_name) || true
  if [ ! -z "$is_raid_device" ]; then
    raid_device_name=$base_device_name
    raid_devices=$(lsblk | grep $raid_device_name -B1 | cut -f1 -d ' ' | grep -v $raid_device_name | tr '\n\r' ' ')
    devices_info=
    for device in $raid_devices
    do
      data_device=$(lsblk -o NAME,SIZE | grep $device | tr -s ' ')
      device_size=$(echo $data_device | cut -f2 -d ' ' | cut -f1 -d "G" | cut -f1 -d ".")
      devices_info="$devices_info $device;$fstype;$device_size"
    done
  fi

  # Discover FS options on device, for now, only supports:
  #  - XFS options: crc finobt
  if [ $fstype = "xfs" ]; then
    xfs_info=$(xfs_info /dev/$base_device_name)
    xfs_option_choices="crc finobt"
    opt_num=0
    opt_sep=""
    for xfs_option_choice in $xfs_option_choices
    do
      opt_grep=$(echo $xfs_info | grep $xfs_option_choice) || true
      if [ ! -z "$opt_grep" ]; then
        let "opt_num=opt_num+1"
        if [ $opt_num -gt 1 ]; then
          opt_sep=","
        fi
        xfs_val=$(echo $opt_grep | sed -e "s/.*$xfs_option_choice=//; s/ .*//")
        xfs_options="$xfs_options$opt_sep$xfs_option_choice=$xfs_val"
      fi
    done
    if [ ! -z "$xfs_options" ]; then
      fs_options="-m $xfs_options"
    fi
  fi
  # Specify the Block devices and sizes to be attached to the EC2 instance.
  for info in $devices_info
  do
    device=$(echo $info | cut -f1 -d ';')
    size=$(echo $info | cut -f3 -d ';')
    device_names=$(echo "$device_names $device" | sed -e 's/^ *//; s/ *$//')
    block_device_option="$block_device_option --blockDevice $device $size"
  done
elif [ "Windows_NT" = "$OS" ]; then
  # We use 'xvdf' as the first available device for the EBS volume, which is mounted on 'd:'
  # See http://docs.aws.amazon.com/AWSEC2/latest/WindowsGuide/device_naming.html
  device="xvdf"
  device_names="d"
  devices_info="$device;$fstype;$device_size"
  block_device_option="--blockDevice $device $device_size"
fi

echo "Devices: $devices_info"

# Launch a new instance.
aws_ec2=$(python buildscripts/aws_ec2.py \
          --ami $ami                     \
          --instanceType $instance_type  \
          --keyName $ssh_key_id          \
          --mode create                  \
          $security_group_ids            \
          $security_groups               \
          $subnet_id                     \
          --tagName "$tag_name"          \
          --tagOwner "$USER"             \
          $expire_tag                    \
          $block_device_option | tr -cd "[:print:]\n")
echo "Spawned new AMI EC2 instance: $aws_ec2"

# Get new instance ID & ip_address
instance_id=$(echo $aws_ec2 | sed -e "s/.*instance_id: //; s/ .*//")
ip_address=$(echo $aws_ec2 | sed -e "s/.*private_ip_address: //; s/ .*//")

# Save AWS information on spawned EC2 instance to be used as an expansion macro.
echo "instance_id: $instance_id" > $aws_ec2_yml
echo "ami: $ami" >> $aws_ec2_yml
echo "instance_type: $instance_type" >> $aws_ec2_yml
echo "ip_address: $ip_address" >> $aws_ec2_yml
echo "device_names: $device_names" >> $aws_ec2_yml
echo "raid_device_name: $raid_device_name" >> $aws_ec2_yml
echo "fstype: $fstype" >> $aws_ec2_yml
echo "fs_options: $fs_options" >> $aws_ec2_yml
