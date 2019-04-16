#!/usr/bin/env python3
"""AWS EC2 instance launcher and controller."""

import base64
import collections
import datetime
import optparse
import sys
import time

import boto3
import botocore
import yaml

_MODES = ("status", "create", "start", "stop", "force-stop", "reboot", "terminate")


def write_binary_file(path, string_buffer):
    """Write string_buffer to path in binary format."""
    with open(path, "wb") as fh:
        fh.write(string_buffer)


def write_utf8_file(path, string_buffer):
    """Write string_buffer to path in utf-8 format."""
    with open(path, "w") as fh:
        fh.write(string_buffer.encode("utf-8"))


def write_yaml_file(path, dictionary):
    """Write dictionary to path in YML format."""
    with open(path, "w") as ystream:
        yaml.safe_dump(dictionary, ystream)


class AwsEc2(object):
    """Class to support controlling AWS EC2 istances."""

    InstanceStatus = collections.namedtuple("InstanceStatus", [
        "instance_id", "image_id", "instance_type", "state", "private_ip_address",
        "public_ip_address", "private_dns_name", "public_dns_name", "tags"
    ])

    def __init__(self):
        """Initialize AwsEc2."""
        try:
            self.connection = boto3.resource("ec2")
        except botocore.exceptions.BotoCoreError:
            print("Please configure your Boto environment variables or files, see"
                  " http://boto3.readthedocs.io/en/latest/guide/configuration.html"
                  " for the variable names, file names and precedence order.")
            raise

    @staticmethod
    def wait_for_state(instance, state, wait_time_secs=0, show_progress=False):
        """Wait up to 'wait_time_secs' for instance to be in 'state'.

        Return 0 if 'state' reached, 1 otherwise.
        """
        if show_progress:
            print("Waiting for instance {} to reach '{}' state".format(instance, state), end="",
                  file=sys.stdout)
        reached_state = False
        end_time = time.time() + wait_time_secs
        while True:
            if show_progress:
                print(".", end="", file=sys.stdout)
                sys.stdout.flush()
            try:
                client_error = ""
                time_left = end_time - time.time()
                instance.load()
                if instance.state["Name"] == state:
                    reached_state = True
                    break
                if time_left <= 0:
                    break
            except botocore.exceptions.ClientError as err:
                # A ClientError exception can sometimes be generated, due to RequestLimitExceeded,
                # so we ignore it and retry until we time out.
                client_error = " {}".format(err.message)

            wait_interval_secs = 15 if time_left > 15 else time_left
            time.sleep(wait_interval_secs)
        if show_progress:
            if reached_state:
                print(" Instance {}!".format(instance.state["Name"]), file=sys.stdout)
            else:
                print(
                    " Instance in state '{}', failed to reach state '{}'{}!".format(
                        instance.state["Name"], state, client_error), file=sys.stdout)
            sys.stdout.flush()
        return 0 if reached_state else 1

    def control_instance(  #pylint: disable=too-many-arguments,too-many-branches,too-many-locals
            self, mode, image_id, wait_time_secs=0, show_progress=False, console_output_file=None,
            console_screenshot_file=None):
        """Control an AMI instance. Returns 0 & status information, if successful."""
        if mode not in _MODES:
            raise ValueError("Invalid mode '{}' specified, choose from {}.".format(mode, _MODES))

        sys.stdout.flush()
        instance = self.connection.Instance(image_id)
        try:
            if mode == "start":
                state = "running"
                instance.start()
            elif mode == "stop":
                state = "stopped"
                instance.stop()
            elif mode == "force-stop":
                state = "stopped"
                instance.stop(Force=True)
            elif mode == "terminate":
                state = "terminated"
                instance.terminate()
            elif mode == "reboot":
                state = "running"
                instance.reboot()
            else:
                state = None
                wait_time_secs = 0
        except botocore.exceptions.ClientError as err:
            return 1, err.message

        ret = 0
        if wait_time_secs > 0:
            ret = self.wait_for_state(instance=instance, state=state, wait_time_secs=wait_time_secs,
                                      show_progress=show_progress)
        try:
            # Always provide status after executing command.
            status = self.InstanceStatus(
                getattr(instance, "instance_id", None), getattr(instance, "image_id", None),
                getattr(instance, "instance_type", None), getattr(instance, "state", None),
                getattr(instance, "private_ip_address", None),
                getattr(instance, "public_ip_address", None),
                getattr(instance, "private_dns_name", None),
                getattr(instance, "public_dns_name", None), getattr(instance, "tags", None))

            if console_output_file:
                try:
                    console_ouput = instance.console_output()
                    if console_ouput and "Output" in console_ouput:
                        write_utf8_file(console_output_file, console_ouput["Output"])
                    else:
                        print("Unable to generate console_ouptut file, data not available")
                except botocore.exceptions.ClientError as err:
                    print("Unable to generate console_ouptut file: {}".format(err.message))

            if console_screenshot_file:
                client = boto3.client("ec2")
                try:
                    console_screenshot = client.get_console_screenshot(InstanceId=image_id)
                    if console_screenshot and "ImageData" in console_screenshot:
                        write_binary_file(console_screenshot_file,
                                          base64.decodestring(console_screenshot["ImageData"]))
                    else:
                        print("Unable to generate console_screenshot file, data not available")
                except botocore.exceptions.ClientError as err:
                    print("Unable to generate console_screenshot file: {}".format(err.message))

        except botocore.exceptions.ClientError as err:
            return 1, err.message

        return ret, status

    def tag_instance(self, image_id, tags):
        """Tag an AMI instance."""
        if tags:
            # It's possible that ClientError code InvalidInstanceID.NotFound could be returned,
            # even if the 'image_id' exists. We will retry up to 5 times, with increasing wait,
            # if this occurs.
            # http://docs.aws.amazon.com/AWSEC2/latest/APIReference/query-api-troubleshooting.html
            for i in range(5):
                try:
                    instance = self.connection.Instance(image_id)
                    break
                except botocore.exceptions.ClientError as err:
                    if err.response["Error"]["Code"] != "InvalidInstanceID.NotFound":
                        raise
                time.sleep(i + 1)
            instance.create_tags(Tags=tags)

    def launch_instance(  # pylint: disable=too-many-arguments,too-many-locals
            self, ami, instance_type, block_devices=None, key_name=None, security_group_ids=None,
            security_groups=None, subnet_id=None, tags=None, wait_time_secs=0, show_progress=False,
            console_output_file=None, console_screenshot_file=None, **kwargs):
        """Launch and tag an AMI instance.

        Return the tuple (0, status_information), if successful.
        """

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
        if security_group_ids:
            kwargs["SecurityGroupIds"] = security_group_ids
        if security_groups:
            kwargs["SecurityGroups"] = security_groups
        if subnet_id:
            kwargs["SubnetId"] = subnet_id
        if key_name:
            kwargs["KeyName"] = key_name

        try:
            instances = self.connection.create_instances(ImageId=ami, InstanceType=instance_type,
                                                         MaxCount=1, MinCount=1, **kwargs)
        except (botocore.exceptions.ClientError, botocore.exceptions.ParamValidationError) as err:
            return 1, err.message

        instance = instances[0]
        if wait_time_secs > 0:
            self.wait_for_state(instance=instance, state="running", wait_time_secs=wait_time_secs,
                                show_progress=show_progress)

        self.tag_instance(instance.instance_id, tags)

        return self.control_instance("status", instance.instance_id,
                                     console_output_file=console_output_file,
                                     console_screenshot_file=console_screenshot_file)


def main():  # pylint: disable=too-many-locals,too-many-statements
    """Execute Main program."""

    required_create_options = ["ami", "key_name"]

    parser = optparse.OptionParser(description=__doc__)
    control_options = optparse.OptionGroup(parser, "Control options")
    create_options = optparse.OptionGroup(parser, "Create options")
    status_options = optparse.OptionGroup(parser, "Status options")

    parser.add_option(
        "--mode", dest="mode", choices=_MODES, default="status",
        help=("Operations to perform on an EC2 instance, choose one of"
              " '{}', defaults to '%default'.".format(", ".join(_MODES))))

    control_options.add_option("--imageId", dest="image_id", default=None,
                               help="EC2 image_id to perform operation on [REQUIRED for control].")

    control_options.add_option(
        "--waitTimeSecs", dest="wait_time_secs", type=int, default=5 * 60,
        help=("Time to wait for EC2 instance to reach it's new state,"
              " defaults to '%default'."))

    create_options.add_option("--ami", dest="ami", default=None,
                              help="EC2 AMI to launch [REQUIRED for create].")

    create_options.add_option(
        "--blockDevice", dest="block_devices", metavar="DEVICE-NAME DEVICE-SIZE-GB",
        action="append", default=[], nargs=2,
        help=("EBS device name and volume size in GiB."
              " More than one device can be attached, by specifying"
              " this option more than once."
              " The device will be deleted on termination of the instance."))

    create_options.add_option("--instanceType", dest="instance_type", default="t1.micro",
                              help="EC2 instance type to launch, defaults to '%default'.")

    create_options.add_option("--keyName", dest="key_name", default=None,
                              help="EC2 key name [REQUIRED for create].")

    create_options.add_option(
        "--securityGroupIds", dest="security_group_ids", action="append", default=[],
        help=("EC2 security group ids. More than one security group id can be"
              " added, by specifying this option more than once."))

    create_options.add_option(
        "--securityGroup", dest="security_groups", action="append", default=[],
        help=("EC2 security group. More than one security group can be added,"
              " by specifying this option more than once."))

    create_options.add_option("--subnetId", dest="subnet_id", default=None,
                              help="EC2 subnet id to use in VPC.")

    create_options.add_option("--tagExpireHours", dest="tag_expire_hours", type=int, default=2,
                              help="EC2 tag expire time in hours, defaults to '%default'.")

    create_options.add_option("--tagName", dest="tag_name", default="",
                              help="EC2 tag and instance name.")

    create_options.add_option("--tagOwner", dest="tag_owner", default="", help="EC2 tag owner.")

    create_options.add_option(
        "--extraArgs", dest="extra_args", metavar="{key1: value1, key2: value2, ..., keyN: valueN}",
        default=None, help=("EC2 create instance keyword args. The argument is specified as"
                            " bracketed YAML - i.e. JSON with support for single quoted"
                            " and unquoted keys. Example, '{DryRun: True}'"))

    status_options.add_option("--yamlFile", dest="yaml_file", default=None,
                              help="Save the status into the specified YAML file.")

    status_options.add_option(
        "--consoleOutputFile", dest="console_output_file", default=None,
        help="Save the console output into the specified file, if"
        " available.")

    status_options.add_option(
        "--consoleScreenshotFile", dest="console_screenshot_file", default=None,
        help="Save the console screenshot (JPG format) into the specified"
        " file, if available.")

    parser.add_option_group(control_options)
    parser.add_option_group(create_options)
    parser.add_option_group(status_options)

    (options, _) = parser.parse_args()

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
            ami=options.ami, instance_type=options.instance_type, block_devices=block_devices,
            key_name=options.key_name, security_group_ids=options.security_group_ids,
            security_groups=options.security_groups, subnet_id=options.subnet_id, tags=tags,
            wait_time_secs=options.wait_time_secs, show_progress=True,
            console_output_file=options.console_output_file,
            console_screenshot_file=options.console_screenshot_file, **my_kwargs)
    else:
        if not getattr(options, "image_id", None):
            parser.print_help()
            parser.error("Missing required control option")

        (ret_code, instance_status) = aws_ec2.control_instance(
            mode=options.mode, image_id=options.image_id, wait_time_secs=options.wait_time_secs,
            show_progress=True, console_output_file=options.console_output_file,
            console_screenshot_file=options.console_screenshot_file)

    if ret_code:
        print("Return code: {}, {}".format(ret_code, instance_status))
        sys.exit(ret_code)

    status_dict = {}
    for field in getattr(instance_status, "_fields", []):
        status_dict[field] = getattr(instance_status, field)

    if options.yaml_file:
        print("Saving status to {}".format(options.yaml_file))
        write_yaml_file(options.yaml_file, status_dict)

    print(yaml.safe_dump(status_dict))


if __name__ == "__main__":
    main()
