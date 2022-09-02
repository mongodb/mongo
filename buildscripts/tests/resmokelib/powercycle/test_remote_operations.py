#!/usr/bin/env python3
"""Unit test for buildscripts/remote_operations.py.

   Note - Tests require sshd to be enabled on localhost with paswordless login
   and can fail otherwise."""

import os
import shutil
import tempfile
import time
import unittest

from buildscripts.resmokelib.powercycle.lib import remote_operations as rop

# pylint: disable=invalid-name


class RemoteOperationsTestCase(unittest.TestCase):
    def setUp(self):
        self.temp_local_dir = tempfile.mkdtemp()
        self.temp_remote_dir = tempfile.mkdtemp()
        self.rop = rop.RemoteOperations(user_host="localhost")
        self.rop_use_shell = rop.RemoteOperations(user_host="localhost", use_shell=True)
        self.rop_sh_shell_binary = rop.RemoteOperations(user_host="localhost",
                                                        shell_binary="/bin/sh")
        self.rop_ssh_opts = rop.RemoteOperations(
            user_host="localhost",
            ssh_connection_options="-v -o ConnectTimeout=10 -o ConnectionAttempts=10")

    def tearDown(self):
        shutil.rmtree(self.temp_local_dir, ignore_errors=True)
        shutil.rmtree(self.temp_remote_dir, ignore_errors=True)


class RemoteOperationConnection(RemoteOperationsTestCase):
    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def runTest(self):

        self.assertTrue(self.rop.access_established())
        ret, buff = self.rop.access_info()
        self.assertEqual(0, ret)

        # Invalid host
        remote_op = rop.RemoteOperations(user_host="badhost")
        ret, buff = remote_op.access_info()
        self.assertFalse(remote_op.access_established())
        self.assertEqual(255, ret)
        self.assertIsNotNone(buff)

        # Valid host with invalid ssh options
        ssh_connection_options = "-o invalid"
        remote_op = rop.RemoteOperations(user_host="localhost",
                                         ssh_connection_options=ssh_connection_options)
        ret, buff = remote_op.access_info()
        self.assertFalse(remote_op.access_established())
        self.assertNotEqual(0, ret)
        self.assertIsNotNone(buff)

        ssh_options = "--invalid"
        remote_op = rop.RemoteOperations(user_host="localhost", ssh_options=ssh_options)
        ret, buff = remote_op.access_info()
        self.assertFalse(remote_op.access_established())
        self.assertNotEqual(0, ret)
        self.assertIsNotNone(buff)

        # Valid host with valid ssh options
        ssh_connection_options = "-v -o ConnectTimeout=10 -o ConnectionAttempts=10"
        remote_op = rop.RemoteOperations(user_host="localhost",
                                         ssh_connection_options=ssh_connection_options)
        ret, buff = remote_op.access_info()
        self.assertTrue(remote_op.access_established())
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ssh_options = "-v -o ConnectTimeout=10 -o ConnectionAttempts=10"
        remote_op = rop.RemoteOperations(user_host="localhost", ssh_options=ssh_options)
        ret, buff = remote_op.access_info()
        self.assertTrue(remote_op.access_established())
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ssh_connection_options = "-v -o ConnectTimeout=10 -o ConnectionAttempts=10"
        ssh_options = "-t"
        remote_op = rop.RemoteOperations(user_host="localhost",
                                         ssh_connection_options=ssh_connection_options,
                                         ssh_options=ssh_options)
        ret, buff = remote_op.access_info()
        self.assertTrue(remote_op.access_established())
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)


class RemoteOperationShell(RemoteOperationsTestCase):
    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def runTest(self):

        # Shell connect
        ret, buff = self.rop.shell("uname")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_use_shell.shell("uname")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_sh_shell_binary.shell("uname")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop.operation("shell", "uname")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        # Invalid command
        ret, buff = self.rop.shell("invalid_command")
        self.assertNotEqual(0, ret)
        self.assertIsNotNone(buff)

        # Multiple commands
        ret, buff = self.rop.shell("date; whoami; ls")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_use_shell.shell("date; whoami; ls")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_sh_shell_binary.shell("date; whoami; ls")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_ssh_opts.shell("date; whoami; ls")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        # Command with single quotes
        ret, buff = self.rop.shell("echo 'hello there' | grep 'hello'")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_use_shell.shell("echo 'hello there' | grep 'hello'")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        # Multiple commands with escaped single quotes
        ret, buff = self.rop.shell("echo \"hello \'dolly\'\"; pwd; echo \"goodbye \'charlie\'\"")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_use_shell.shell(
            "echo \"hello \'dolly\'\"; pwd; echo \"goodbye \'charlie\'\"")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        # Command with escaped double quotes
        ret, buff = self.rop.shell("echo \"hello there\" | grep \"hello\"")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_use_shell.shell("echo \"hello there\" | grep \"hello\"")
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        # Command with directory and pipe
        ret, buff = self.rop.shell("touch {dir}/{file}; ls {dir} | grep {file}".format(
            file=time.time(), dir="/tmp"))
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)

        ret, buff = self.rop_use_shell.shell("touch {dir}/{file}; ls {dir} | grep {file}".format(
            file=time.time(), dir="/tmp"))
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)


class RemoteOperationCopyTo(RemoteOperationsTestCase):
    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def runTest(self):

        # Copy to remote
        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop.copy_to(l_temp_path, self.temp_remote_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        r_temp_path = os.path.join(self.temp_remote_dir, l_temp_file)
        self.assertTrue(os.path.isfile(r_temp_path))

        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop_use_shell.copy_to(l_temp_path, self.temp_remote_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        r_temp_path = os.path.join(self.temp_remote_dir, l_temp_file)
        self.assertTrue(os.path.isfile(r_temp_path))

        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop.operation("copy_to", l_temp_path, self.temp_remote_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        self.assertTrue(os.path.isfile(r_temp_path))

        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop_ssh_opts.operation("copy_to", l_temp_path, self.temp_remote_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        self.assertTrue(os.path.isfile(r_temp_path))

        # Copy multiple files to remote
        num_files = 3
        l_temp_files = []
        for i in range(num_files):
            l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
            l_temp_file = os.path.basename(l_temp_path)
            l_temp_files.append(l_temp_path)
        ret, buff = self.rop.copy_to(" ".join(l_temp_files), self.temp_remote_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        for i in range(num_files):
            r_temp_path = os.path.join(self.temp_remote_dir, os.path.basename(l_temp_files[i]))
            self.assertTrue(os.path.isfile(r_temp_path))

        num_files = 3
        l_temp_files = []
        for i in range(num_files):
            l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
            l_temp_file = os.path.basename(l_temp_path)
            l_temp_files.append(l_temp_path)
        ret, buff = self.rop_use_shell.copy_to(" ".join(l_temp_files), self.temp_remote_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        for i in range(num_files):
            r_temp_path = os.path.join(self.temp_remote_dir, os.path.basename(l_temp_files[i]))
            self.assertTrue(os.path.isfile(r_temp_path))

        # Copy to remote without directory
        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop.copy_to(l_temp_path)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        r_temp_path = os.path.join(os.environ["HOME"], l_temp_file)
        self.assertTrue(os.path.isfile(r_temp_path))
        os.remove(r_temp_path)

        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop_use_shell.copy_to(l_temp_path)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        r_temp_path = os.path.join(os.environ["HOME"], l_temp_file)
        self.assertTrue(os.path.isfile(r_temp_path))
        os.remove(r_temp_path)

        # Copy to remote with space in file name, note it must be quoted.
        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir, prefix="filename with space")[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop.copy_to("'{}'".format(l_temp_path))
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        r_temp_path = os.path.join(os.environ["HOME"], l_temp_file)
        self.assertTrue(os.path.isfile(r_temp_path))
        os.remove(r_temp_path)

        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir, prefix="filename with space")[1]
        l_temp_file = os.path.basename(l_temp_path)
        ret, buff = self.rop_use_shell.copy_to("'{}'".format(l_temp_path))
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        r_temp_path = os.path.join(os.environ["HOME"], l_temp_file)
        self.assertTrue(os.path.isfile(r_temp_path))
        os.remove(r_temp_path)

        # Valid scp options
        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        l_temp_file = os.path.basename(l_temp_path)
        scp_options = "-l 5000"
        remote_op = rop.RemoteOperations(user_host="localhost", scp_options=scp_options)
        ret, buff = remote_op.copy_to(l_temp_path, self.temp_remote_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        r_temp_path = os.path.join(self.temp_remote_dir, l_temp_file)
        self.assertTrue(os.path.isfile(r_temp_path))

        # Invalid scp options
        l_temp_path = tempfile.mkstemp(dir=self.temp_local_dir)[1]
        scp_options = "--invalid"
        remote_op = rop.RemoteOperations(user_host="localhost", scp_options=scp_options)
        ret, buff = remote_op.copy_to(l_temp_path, self.temp_remote_dir)
        self.assertNotEqual(0, ret)
        self.assertIsNotNone(buff)


class RemoteOperationCopyFrom(RemoteOperationsTestCase):
    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def runTest(self):

        # Copy from remote
        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
        r_temp_file = os.path.basename(r_temp_path)
        ret, buff = self.rop.copy_from(r_temp_path, self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        l_temp_path = os.path.join(self.temp_local_dir, r_temp_file)
        self.assertTrue(os.path.isfile(l_temp_path))

        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
        r_temp_file = os.path.basename(r_temp_path)
        ret, buff = self.rop_use_shell.copy_from(r_temp_path, self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        l_temp_path = os.path.join(self.temp_local_dir, r_temp_file)
        self.assertTrue(os.path.isfile(l_temp_path))

        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
        r_temp_file = os.path.basename(r_temp_path)
        ret, buff = self.rop_ssh_opts.copy_from(r_temp_path, self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        l_temp_path = os.path.join(self.temp_local_dir, r_temp_file)
        self.assertTrue(os.path.isfile(l_temp_path))

        # Copy from remote without directory
        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
        r_temp_file = os.path.basename(r_temp_path)
        ret, buff = self.rop.copy_from(r_temp_path)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        self.assertTrue(os.path.isfile(r_temp_file))
        os.remove(r_temp_file)

        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
        r_temp_file = os.path.basename(r_temp_path)
        ret, buff = self.rop_use_shell.copy_from(r_temp_path)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        self.assertTrue(os.path.isfile(r_temp_file))
        os.remove(r_temp_file)

        # Copy from remote with space in file name, note it must be quoted.
        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir, prefix="filename with space")[1]
        r_temp_file = os.path.basename(r_temp_path)
        ret, buff = self.rop.copy_from("'{}'".format(r_temp_path))
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        self.assertTrue(os.path.isfile(r_temp_file))
        os.remove(r_temp_file)

        # Copy multiple files from remote
        num_files = 3
        r_temp_files = []
        for i in range(num_files):
            r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
            r_temp_file = os.path.basename(r_temp_path)
            r_temp_files.append(r_temp_path)
        ret, buff = self.rop.copy_from(" ".join(r_temp_files), self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        for i in range(num_files):
            basefile_name = os.path.basename(r_temp_files[i])
            l_temp_path = os.path.join(self.temp_local_dir, basefile_name)
            self.assertTrue(os.path.isfile(l_temp_path))

        num_files = 3
        r_temp_files = []
        for i in range(num_files):
            r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
            r_temp_file = os.path.basename(r_temp_path)
            r_temp_files.append(r_temp_path)
        ret, buff = self.rop_use_shell.copy_from(" ".join(r_temp_files), self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        for i in range(num_files):
            basefile_name = os.path.basename(r_temp_files[i])
            l_temp_path = os.path.join(self.temp_local_dir, basefile_name)
            self.assertTrue(os.path.isfile(l_temp_path))

        # Copy files from remote with wilcard
        num_files = 3
        r_temp_files = []
        for i in range(num_files):
            r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir, prefix="wild1")[1]
            r_temp_file = os.path.basename(r_temp_path)
            r_temp_files.append(r_temp_path)
        r_temp_path = os.path.join(self.temp_remote_dir, "wild1*")
        ret, buff = self.rop.copy_from(r_temp_path, self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        for i in range(num_files):
            l_temp_path = os.path.join(self.temp_local_dir, os.path.basename(r_temp_files[i]))
            self.assertTrue(os.path.isfile(l_temp_path))

        num_files = 3
        r_temp_files = []
        for i in range(num_files):
            r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir, prefix="wild2")[1]
            r_temp_file = os.path.basename(r_temp_path)
            r_temp_files.append(r_temp_path)
        r_temp_path = os.path.join(self.temp_remote_dir, "wild2*")
        ret, buff = self.rop_use_shell.copy_from(r_temp_path, self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        for i in range(num_files):
            l_temp_path = os.path.join(self.temp_local_dir, os.path.basename(r_temp_files[i]))
            self.assertTrue(os.path.isfile(l_temp_path))

        # Local directory does not exist.
        self.assertRaises(ValueError, lambda: self.rop_use_shell.copy_from(r_temp_path, "bad_dir"))

        # Valid scp options
        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
        r_temp_file = os.path.basename(r_temp_path)
        scp_options = "-l 5000"
        remote_op = rop.RemoteOperations(user_host="localhost", scp_options=scp_options)
        ret, buff = remote_op.copy_from(r_temp_path, self.temp_local_dir)
        self.assertEqual(0, ret)
        self.assertIsNotNone(buff)
        l_temp_path = os.path.join(self.temp_local_dir, r_temp_file)
        self.assertTrue(os.path.isfile(l_temp_path))

        # Invalid scp options
        r_temp_path = tempfile.mkstemp(dir=self.temp_remote_dir)[1]
        scp_options = "--invalid"
        remote_op = rop.RemoteOperations(user_host="localhost", scp_options=scp_options)
        ret, buff = remote_op.copy_from(r_temp_path, self.temp_local_dir)
        self.assertNotEqual(0, ret)
        self.assertIsNotNone(buff)


class RemoteOperation(RemoteOperationsTestCase):
    @unittest.skip("Known broken. SERVER-48969 tracks re-enabling.")
    def runTest(self):
        # Invalid operation
        self.assertRaises(ValueError, lambda: self.rop.operation("invalid", None))
