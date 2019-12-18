"""Unit tests for the client.evergreen module."""

import unittest

import client.evergreen as evergreen

# pylint: disable=missing-docstring


class GenerateEvergreenProjectNameTest(unittest.TestCase):
    def test_generate_evergreen_project_name(self):
        owner = "owner"
        project = "project"
        branch = "branch"

        project_name = evergreen.generate_evergreen_project_name(owner, project, branch)

        self.assertIn(owner, project_name)
        self.assertIn(project, project_name)
        self.assertIn(branch, project_name)
