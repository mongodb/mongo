"""Unit tests for the buildscripts.git module."""

from __future__ import absolute_import

import subprocess
import unittest

import buildscripts.git as _git


class TestRepository(unittest.TestCase):

    def setUp(self):
        self.subprocess = MockSubprocess()
        _git.subprocess = self.subprocess

    def tearDown(self):
        _git.subprocess = subprocess

    def test_base_git_methods(self):
        params = ["param1", "param2", "param3"]
        repo = _git.Repository("/tmp")
        self._check_gito_command(repo.git_add, "add", params)
        self._check_gito_command(repo.git_commit, "commit", params)
        self._check_gito_command(repo.git_diff, "diff", params)
        self._check_gito_command(repo.git_log, "log", params)
        self._check_gito_command(repo.git_push, "push", params)
        self._check_gito_command(repo.git_fetch, "fetch", params)
        self._check_gito_command(repo.git_ls_files, "ls-files", params)
        self._check_gito_command(repo.git_rev_parse, "rev-parse", params)
        self._check_gito_command(repo.git_rm, "rm", params)
        self._check_gito_command(repo.git_show, "show", params)

    def test_base_gito_methods_errors(self):
        params = ["param1", "param2", "param3"]
        repo = _git.Repository("/tmp")
        self._check_gito_command_error(repo.git_add, "add", params)
        self._check_gito_command_error(repo.git_commit, "commit", params)
        self._check_gito_command_error(repo.git_diff, "diff", params)
        self._check_gito_command_error(repo.git_log, "log", params)
        self._check_gito_command_error(repo.git_push, "push", params)
        self._check_gito_command_error(repo.git_fetch, "fetch", params)
        self._check_gito_command_error(repo.git_ls_files, "ls-files", params)
        self._check_gito_command_error(repo.git_rev_parse, "rev-parse", params)
        self._check_gito_command_error(repo.git_rm, "rm", params)
        self._check_gito_command_error(repo.git_show, "show", params)

    def _check_gito_command(self, method, command, params):
        # Initialize subprocess mock.
        self.subprocess.call_output_args = None
        self.subprocess.call_output = str(method)
        self.subprocess.call_returncode = 0
        # Call method.
        value = method(params)
        # Check.
        args = self.subprocess.call_args
        given_args = [command] + params
        self.assertEqual("git", args[0])
        self.assertEqual(given_args, args[-len(given_args):])
        self.assertEqual(str(method), value)

    def _check_gito_command_error(self, method, command, params):
        self.subprocess.call_args = None
        self.subprocess.call_output = None
        self.subprocess.call_returncode = 1

        with self.assertRaises(_git.GitException):
            method(params)
        args = self.subprocess.call_args
        given_args = [command] + params
        self.assertEqual("git", args[0])
        self.assertEqual(given_args, args[-len(given_args):])


class MockSubprocess(object):
    PIPE = subprocess.PIPE
    CalledProcessError = subprocess.CalledProcessError

    def __init__(self):
        self.call_args = None
        self.call_returncode = 0
        self.call_output = ""

    def Popen(self, args, **kwargs):
        self.call_args = args
        return MockProcess(self.call_returncode, self.call_output)


class MockProcess(object):
    def __init__(self, returncode, output):
        self.returncode = returncode
        self._output = output

    def communicate(self):
        return self._output, ""
