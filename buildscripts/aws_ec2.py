#!/usr/bin/env python

"""AWS EC2 instance launcher and controller."""

from __future__ import print_function

import collections
import datetime
import optparse
import sys
import time

import boto3
import botocore
import yaml

_MODES = ("status", "create", "start", "stop", "force-stop", "reboot", "terminate")


class AwsEc2(object):
    """Class to support controlling AWS EC2 istances."""

    InstanceStatus = collections.namedtuple(
        "InstanceStatus",
        "instance_id image_id instance_type state private_ip_address public_ip_address tags")

    def __init__(self):
        try:
            self.connection = boto3.resource("ec2")
        except botocore.exceptions.BotoCoreError:
            print("Please configure your Boto environment variables or files, see"
                  " http://boto3.readthedocs.io/en/latest/guide/configuration.html"
                  " for the variable names, file names and precedence order.")
            raise

    def control_instance(self, mode, image_id):
        """Controls an AMI instance. Returns 0 & status information, if successful."""
        if mode not in _MODES:
            raise ValueError(
                "Invalid mode '{}' specified, choose from {}.".format(mode, _MODES))

        instance = self.connection.Instance(image_id)
        try:
            if mode == "start":
                instance.start()
            elif mode == "stop":
                instance.stop()
            elif mode == "force-stop":
                instance.stop(Force=True)
            elif mode == "terminate":
                instance.terminate()
            elif mode == "reboot":
                instance.reboot()
        except botocore.exceptions.ClientError as e:
            return 1, e.message

        try:
            # Always provide status after executing command.
            status = self.InstanceStatus(
                getattr(instance, "instance_id", None),
                getattr(instance, "image_id", None),
                getattr(instance, "instance_type", None),
                getattr(instance, "state", None),
                getattr(instance, "private_ip_address", None),
                getattr(instance, "public_ip_address", None),
                getattr(instance, "tags", None))
        except botocore.exceptions.ClientError as e:
            return 1, e.message

        return 0, status

    def tag_instance(self, image_id, tags):
        """Tags an AMI instance. """
        if tags:
            # It's possible that ClientError code InvalidInstanceID.NotFound could be returned,
            # even if the 'image_id' exists. We will retry up to 5 times, with increasing wait,
            # if this occurs.
            # http://docs.aws.amazon.com/AWSEC2/latest/APIReference/query-api-troubleshooting.html
            for i in range(5):
                try:
                    instance = self.connection.Instance(image_id)
                    break
                except botocore.exceptions.ClientError as e:
                    if e.response["Error"]["Code"] != "InvalidInstanceID.NotFound":
                        raise
                time.sleep(i + 1)
            instance.create_tags(Tags=tags)

    def launch_instance(self,
                        ami,
                        instance_type,
                        block_devices=None,
                        key_name=None,
                        security_groups=None,
                        tags=None,
                        wait_time_secs=0,
                        show_progress=False,
                        **kwargs):
        """Launches and tags an AMI instance.

           Returns the tuple (0, status_information), if successful."""

        bdms = []
        if block_devices is None:
            block_devices = {}
        for block_device in block_devices:
            bdm = {}
            bdm["DeviceName"] = block_device
            bdm["Ebs"] = {"DeleteOnTermination": True, "VolumeSize": block_devices[block_device]}
            bdms.append(bdm)
        if bdms:
            kwargs["BlockDeviceMappings"] = bdms
        if security_groups:
            kwargs["SecurityGroups"] = security_groups
        if key_name:
            kwargs["KeyName"] = key_name

        try:
            instances = self.connection.create_instances(
                ImageId=ami,
                InstanceType=instance_type,
                MaxCount=1,
                MinCount=1,
                **kwargs)
        except (botocore.exceptions.ClientError, botocore.exceptions.ParamValidationError) as e:
            return 1, e.message

        instance = instances[0]

        if wait_time_secs:
            # Wait up to 'wait_time_secs' for instance to be 'running'.
            end_time = time.time() + wait_time_secs
            if show_progress:
                print("Waiting for instance {} ".format(instance), end="", file=sys.stdout)
            while time.time() < end_time:
                if show_progress:
                    print(".", end="", file=sys.stdout)
                    sys.stdout.flush()
                time.sleep(5)
                instance.load()
                if instance.state["Name"] == "running":
                    if show_progress:
                        print(" Instance running!", file=sys.stdout)
                        sys.stdout.flush()
                    break

        self.tag_instance(instance.instance_id, tags)

        return self.control_instance("status", instance.instance_id)


def main():

    required_create_options = ["ami", "key_name"]

    parser = optparse.OptionParser(description=__doc__)
    control_options = optparse.OptionGroup(parser, "Control options")
    create_options = optparse.OptionGroup(parser, "Create options")

    parser.add_option("--mode",
                      dest="mode",
                      choices=_MODES,
                      default="status",
                      help="Operations to perform on an EC2 instance, choose one of"
                           " '{}', defaults to '%default'.".format(", ".join(_MODES)))

    control_options.add_option("--imageId",
                               dest="image_id",
                               default=None,
                               help="EC2 image_id to perform operation on [REQUIRED for control].")

    create_options.add_option("--ami",
                              dest="ami",
                              default=None,
                              help="EC2 AMI to launch [REQUIRED for create].")

    create_options.add_option("--blockDevice",
                              dest="block_devices",
                              metavar="DEVICE-NAME DEVICE-SIZE-GB",
                              action="append",
                              default=[],
                              nargs=2,
                              help="EBS device name and volume size in GiB."
                                   " More than one device can be attached, by specifying"
                                   " this option more than once."
                                   " The device will be deleted on termination of the instance.")

    create_options.add_option("--instanceType",
                              dest="instance_type",
                              default="t1.micro",
                              help="EC2 instance type to launch, defaults to '%default'.")

    create_options.add_option("--keyName",
                              dest="key_name",
                              default=None,
                              help="EC2 key name [REQUIRED for create].")

    create_options.add_option("--securityGroup",
                              dest="security_groups",
                              action="append",
                              default=[],
                              help="EC2 security group. More than one security group can be added,"
                                   " by specifying this option more than once.")

    create_options.add_option("--tagExpireHours",
                              dest="tag_expire_hours",
                              type=int,
                              default=2,
                              help="EC2 tag expire time in hours, defaults to '%default'.")

    create_options.add_option("--tagName",
                              dest="tag_name",
                              default="",
                              help="EC2 tag and instance name.")

    create_options.add_option("--tagOwner",
                              dest="tag_owner",
                              default="",
                              help="EC2 tag owner.")

    create_options.add_option("--extraArgs",
                              dest="extra_args",
                              metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
                              default=None,
                              help="EC2 create instance keyword args. The argument is specified as"
                                   " bracketed YAML - i.e. JSON with support for single quoted"
                                   " and unquoted keys. Example, '{DryRun: True}'")

    parser.add_option_group(control_options)
    parser.add_option_group(create_options)

    (options, args) = parser.parse_args()

    aws_ec2 = AwsEc2()

    if options.mode == "create":
        for option in required_create_options:
            if not getattr(options, option, None):
                parser.print_help()
                parser.error("Missing required create option")

        block_devices = {}
        for (device_name, device_size) in options.block_devices:
            try:
                device_size = int(device_size)
            except ValueError:
                parser.print_help()
                parser.error("Block size must be an integer")
            block_devices[device_name] = device_size

        # The 'expire-on' key is a UTC time.
        expire_dt = datetime.datetime.utcnow() + datetime.timedelta(hours=options.tag_expire_hours)
        tags = [{"Key": "expire-on", "Value": expire_dt.strftime("%Y-%m-%d %H:%M:%S")},
                {"Key": "Name", "Value": options.tag_name},
                {"Key": "owner", "Value": options.tag_owner}]

        my_kwargs = {}
        if options.extra_args is not None:
            my_kwargs = yaml.safe_load(options.extra_args)

        (ret_code, instance_status) = aws_ec2.launch_instance(
            ami=options.ami,
            instance_type=options.instance_type,
            block_devices=block_devices,
            key_name=options.key_name,
            security_groups=options.security_groups,
            tags=tags,
            wait_time_secs=60,
            show_progress=True,
            **my_kwargs)
    else:
        if not getattr(options, "image_id", None):
            parser.print_help()
            parser.error("Missing required control option")

        (ret_code, instance_status) = aws_ec2.control_instance(options.mode, options.image_id)

    print("Return code: {}, Instance status:".format(ret_code))
    if ret_code:
        print(instance_status)
    else:
        for field in instance_status._fields:
            print("\t{}: {}".format(field, getattr(instance_status, field)))

    sys.exit(ret_code)

if __name__ == "__main__":
    main()
