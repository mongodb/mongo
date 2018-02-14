#!/usr/bin/env python

"""Unit test for buildscripts/aws_ec2.py."""

import datetime
import os
import sys
import unittest

if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.getcwd())
from buildscripts import aws_ec2

_AMI = "ami-ed6bec86"
_INSTANCE_TYPE = "t1.micro"


class AwsEc2TestCase(unittest.TestCase):
    def setUp(self):
        self.aws_ec2 = aws_ec2.AwsEc2()
        self.launched_instances = []
        self.ami = _AMI
        self.instance_type = _INSTANCE_TYPE
        self.key_name = None
        self.security_groups = None
        self.expire_dt = datetime.datetime.utcnow() + datetime.timedelta(hours=1)
        self.tags = [{"Key": "expire-on", "Value": self.expire_dt.strftime("%Y-%m-%d %H:%M:%S")},
                     {"Key": "Name", "Value": "Unittest AWS EC2 Launcher"},
                     {"Key": "owner", "Value": ""}]

    def tearDown(self):
        for instance in self.launched_instances:
            self.aws_ec2.control_instance(mode="terminate", image_id=instance)


class AwsEc2Connect(AwsEc2TestCase):
    def runTest(self):
        self.assertIsNotNone(self.aws_ec2)


class AwsEc2Launch(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            block_devices={"xvde": 5, "xvdf": 10},
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags,
            DryRun=True)
        self.assertEqual(1, code, ret)

        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags,
            InvalidParam=True)
        self.assertEqual(1, code, ret)

        code, ret = self.aws_ec2.launch_instance(
            ami="ami-bad_ami",
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertNotEqual(0, code, ret)

        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type="bad_instance_type",
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertNotEqual(0, code, ret)

        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name="bad_key_name",
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertNotEqual(0, code, ret)


class AwsEc2LaunchStatus(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)
        self.assertEqual(self.ami, ret.image_id, ret)
        self.assertEqual(self.instance_type, ret.instance_type, ret)
        self.assertTrue(ret.state["Name"] in ["pending", "running"], ret)
        for tag in ret.tags:
            self.assertTrue(tag in self.tags, ret)
        for tag in self.tags:
            self.assertTrue(tag in ret.tags, ret)

        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags,
            wait_time_secs=300,
            show_progress=True)
        self.assertEqual("running", ret.state["Name"], ret)
        self.assertIsNotNone(ret.public_ip_address, ret)
        self.assertIsNotNone(ret.private_ip_address, ret)
        self.launched_instances.append(ret.instance_id)


class AwsEc2ControlStatus(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        code, ret = self.aws_ec2.control_instance(
            mode="status",
            image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertEqual(self.ami, ret.image_id, ret)
        self.assertEqual(self.instance_type, ret.instance_type, ret)
        self.assertTrue(ret.state["Name"] in ["pending", "running"], ret)
        for tag in ret.tags:
            self.assertTrue(tag in self.tags, ret)
        for tag in self.tags:
            self.assertTrue(tag in ret.tags, ret)
        self.assertIsNotNone(ret.instance_id, ret)
        self.assertIsNotNone(ret.image_id, ret)
        self.assertIsNotNone(ret.instance_type, ret)
        self.assertIsNotNone(ret.state["Name"], ret)
        self.assertIsNotNone(ret.tags, ret)

        self.assertRaises(ValueError,
                          self.aws_ec2.control_instance,
                          mode="bad_mode",
                          image_id=ret.instance_id)

        code, ret = self.aws_ec2.control_instance(mode="status", image_id="bad_id")
        self.assertNotEqual(0, code, ret)
        self.assertRegexpMatches(ret, "Invalid", ret)


class AwsEc2ControlStart(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        code, ret = self.aws_ec2.control_instance(mode="start", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertTrue(ret.state["Name"] in ["starting", "running"], ret)


class AwsEc2ControlStartReboot(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        code, ret = self.aws_ec2.control_instance(mode="start", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertTrue(ret.state["Name"] in ["running"], ret)

        code, ret = self.aws_ec2.control_instance(mode="reboot", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertTrue(ret.state["Name"] in ["rebooting", "running"], ret)


class AwsEc2ControlStop(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags,
            wait_time_secs=60)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        code, ret = self.aws_ec2.control_instance(mode="stop", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertTrue(ret.state["Name"] in ["stopping", "stopped"], ret)

        code, ret = self.aws_ec2.control_instance(mode="start", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertTrue(ret.state["Name"] in ["pending", "running"], ret)

        code, ret = self.aws_ec2.control_instance(mode="force-stop", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertTrue(ret.state["Name"] in ["stopping", "stopped"], ret)


class AwsEc2ControlTerminate(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=self.tags)
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        code, ret = self.aws_ec2.control_instance(mode="terminate", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        self.assertTrue(ret.state["Name"] in ["shutting-down", "terminated"], ret)

        code, ret = self.aws_ec2.control_instance(mode="start", image_id=ret.instance_id)
        self.assertNotEqual(0, code, ret)


class AwsEc2TagInstance(AwsEc2TestCase):
    def runTest(self):
        code, ret = self.aws_ec2.launch_instance(
            ami=self.ami,
            instance_type=self.instance_type,
            key_name=self.key_name,
            security_groups=self.security_groups,
            tags=[])
        self.assertEqual(0, code, ret)
        self.launched_instances.append(ret.instance_id)

        self.aws_ec2.tag_instance(image_id=ret.instance_id, tags=self.tags)
        code, ret = self.aws_ec2.control_instance(mode="status", image_id=ret.instance_id)
        self.assertEqual(0, code, ret)
        for tag in ret.tags:
            self.assertTrue(tag in self.tags, ret)
        for tag in self.tags:
            self.assertTrue(tag in ret.tags, ret)


if __name__ == "__main__":
    unittest.main()
