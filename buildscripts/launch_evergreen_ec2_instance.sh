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
  -k <ssh_key_id>,        [REQUIRED] The ssh key id used to access the new AWS EC2 instance.
  -y <aws_ec2_yml>,       [REQUIRED] YAML file name where to store the new AWS EC2 instance
                          information. This file will be used in etc/evergreen.yml for
                          macro expansion of variables used in other functions.
  -e <expire_hours>,      [OPTIONAL] Number of hours to expire the AWS EC2 instance.
  -g <security_group_id>, [OPTIONAL] The security group id to be used for the new AWS EC2 instance.
                          To specify more than one group, invoke this option each time.
  -n <subnet_id>,         [OPTIONAL] The subnet id in which to launch the AWS EC2 instance.
  -s <security_group>,    [OPTIONAL] The security group to be used for the new AWS EC2 instance.
                          To specify more than one group, invoke this option each time.
  -t <tag_name>,          [OPTIONAL] The tag name of the new AWS EC2 instance.
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
base_data_device_name=$(echo $data_df | cut -f1 -d ' ' | sed -e 's/.*\///g')
fstype=$(echo $data_df | cut -f2 -d ' ')
# Set the size for /data to 100GB.
let data_device_size=100
if [[ -z "$base_data_device_name" || -z "$fstype" ]]; then
  echo "Could not detect /data mount point, one of the following are not detected:"
  echo "base_device_name: '$base_data_device_name' fstype: '$fstype' data_device_size: '$data_device_size'"
  exit 1
fi

# Linux variants have block device mappings, so discover the mounted drives.
if [ $(uname) = "Linux" ]; then
  data_devices_info="$base_data_device_name;$fstype;$data_device_size"

  is_raid_device=$(grep -s active /proc/mdstat | grep $base_data_device_name) || true
  if [ ! -z "$is_raid_device" ]; then
    raid_data_device_name=$base_data_device_name
    raid_data_devices=$(lsblk | grep $raid_data_device_name -B1 | cut -f1 -d ' ' | grep -v $raid_data_device_name | tr '\n\r' ' ')
    data_devices_info=
    for data_device in $raid_data_devices
    do
      data_device=$(lsblk -o NAME,SIZE | grep $data_device | tr -s ' ')
      data_devices_info="$data_devices_info $data_device;$fstype;$data_device_size"
    done
  fi

  # Discover FS options on device, for now, only supports:
  #  - XFS options: crc finobt
  if [ $fstype = "xfs" ]; then
    xfs_info=$(xfs_info /dev/$base_data_device_name)
    xfs_option_choices="crc finobt"
    opt_num=0
    opt_sep=""
    for xfs_option_choice in $xfs_option_choices
    do
      opt_grep=$(echo $xfs_info | grep $xfs_option_choice) || true
      if [ ! -z "$opt_grep" ]; then
        let opt_num=opt_num+1
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
  for info in $data_devices_info
  do
    data_device=$(echo $info | cut -f1 -d ';')
    data_size=$(echo $info | cut -f3 -d ';')
    data_device_names=$(echo "$data_device_names $data_device" | sed -e 's/^ *//; s/ *$//')
    block_data_device_option="$block_data_device_option --blockDevice $data_device $data_size"
  done
  # We use next available device for the logging EBS volume
  last_data_device=$(echo ${data_device_names##* })
  next_drive_letter=$(echo -n $last_data_device | tail -c 1 | tr "0-9a-z" "1-9a-z_")
  log_device_name="${last_data_device%?}$next_drive_letter"
  log_device_size=50
  log_devices_info="$log_device_name;$fstype;$log_device_size"
  block_log_device_option="--blockDevice $log_device_name $log_device_size"
elif [ "Windows_NT" = "$OS" ]; then
  # We use 'xvdf' as the first available device for the logging EBS volume, which is mounted on 'd:'
  # See http://docs.aws.amazon.com/AWSEC2/latest/WindowsGuide/device_naming.html
  data_device="xvdf"
  data_device_names="d"
  data_devices_info="$data_device;$fstype;$data_device_size"
  block_data_device_option="--blockDevice $data_device $data_device_size"
  # We use 'xvdg' as the next available device for the logging EBS volume, which is mounted on 'e:'
  log_device="xvdg"
  log_device_name="e"
  log_device_size=50
  log_devices_info="$log_device;$fstype;$log_device_size"
  block_log_device_option="--blockDevice $log_device $log_device_size"
fi

echo "Data Devices: $data_devices_info"

# Launch a new instance.
aws_ec2_status_yml=aws_ec2_status.yml
aws_ec2=$(python buildscripts/aws_ec2.py \
          --yamlFile $aws_ec2_status_yml \
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
          $block_log_device_option       \
          $block_data_device_option | tr -cd "[:print:]\n")
aws_status=$?
echo "Spawned new AMI EC2 instance: $aws_ec2"

# Read the attributes from $aws_ec2_status_yml and save in $aws_ec2_yml.
> $aws_ec2_yml
ec2_attributes="instance_id private_ip_address"
for ec2_attribute in $ec2_attributes
do
  ec2_value=$(python buildscripts/yaml_key_value.py --yamlFile $aws_ec2_status_yml --yamlKey $ec2_attribute)
  # Only save the ec2_attribute if it's defined.
  if [ -n "$ec2_value" ]; then
    echo "$ec2_attribute: $ec2_value" >> $aws_ec2_yml
  fi
done

# Save additional AWS information on spawned EC2 instance to be used as an expansion macro.
options="data_device_names raid_data_device_name log_device_name fstype fs_options"
for option in $options
do
  # Only save the option if it's defined.
  if [ -n "${!option}" ]; then
    echo "$option: ${!option}" >> $aws_ec2_yml
  fi
done

exit $aws_status
