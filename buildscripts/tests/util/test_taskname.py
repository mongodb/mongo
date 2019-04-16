"""Unit tests for the util/taskname.py script."""

import unittest

from buildscripts.util import taskname

# pylint: disable=missing-docstring,protected-access


class TestNameTask(unittest.TestCase):
    def test_name_task_with_width_one(self):
        self.assertEqual("name_3_var", taskname.name_generated_task("name", 3, 10, "var"))

    def test_name_task_with_width_four(self):
        self.assertEqual("task_3141_var", taskname.name_generated_task("task", 3141, 5000, "var"))
