"""Unit tests for the util/taskname.py script."""

import unittest

import buildscripts.util.taskname as under_test

# pylint: disable=missing-docstring,protected-access


class TestNameTask(unittest.TestCase):
    def test_name_task_with_width_one(self):
        self.assertEqual("name_3_var", under_test.name_generated_task("name", 3, 10, "var"))

    def test_name_task_with_width_four(self):
        self.assertEqual("task_3141_var", under_test.name_generated_task("task", 3141, 5000, "var"))


class TestRemoveGenSuffix(unittest.TestCase):
    def test_removes_gen_suffix(self):
        input_task_name = "sharding_auth_auditg_gen"
        self.assertEqual("sharding_auth_auditg", under_test.remove_gen_suffix(input_task_name))

    def test_doesnt_remove_non_gen_suffix(self):
        input_task_name = "sharded_multi_stmt_txn_jscore_passthroug"
        self.assertEqual("sharded_multi_stmt_txn_jscore_passthroug",
                         under_test.remove_gen_suffix(input_task_name))
