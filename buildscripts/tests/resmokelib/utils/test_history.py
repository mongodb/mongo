"""Unit tests for buildscripts/resmokelib/utils/scheduler.py."""

import sched
import unittest

from buildscripts.resmokelib.utils.history import HistoryDict, make_historic


class TestHistory(unittest.TestCase):
    """Unit tests for the HistoryDict class."""

    def test_acts_like_dict(self):
        test_dict = HistoryDict()
        self.assertRaises(KeyError, lambda: test_dict["nonexistent_key"])

        test_dict["key1"] = "key1value1"
        self.assertEqual(test_dict["key1"], "key1value1")
        test_dict["key1"] = "key1value2"
        self.assertEqual(test_dict["key1"], "key1value2")
        del test_dict["key1"]
        self.assertRaises(KeyError, lambda: test_dict["key1"])

    def test_is_iterable(self):
        test_dict = HistoryDict()
        test_dict["key1"] = "key1val"
        test_dict["key2"] = "key2val"
        test_dict["key3"] = "key3val"
        test_dict["key4"] = "key4val"

        expected_vals = ["key1val", "key2val", "key3val", "key4val"]
        actual_vals = []
        for key in test_dict:
            actual_vals.append(test_dict[key])

        self.assertCountEqual(actual_vals, expected_vals)

    def test_inner_dict(self):
        test_dict = HistoryDict()
        inner_dict = HistoryDict()
        inner_dict["foo"] = "bar"
        test_dict["innerDict"] = inner_dict
        test_dict["innerDict"]["foo"] = "za"
        another_ref = test_dict["innerDict"]
        another_ref["another_added"] = "another_val"
        self.assertEqual(test_dict["innerDict"]["foo"], "za")
        self.assertEqual(test_dict["innerDict"]["another_added"], "another_val")

        expected_test_dict = """SchemaVersion: "0.1"


History:


  innerDict:
  - time: 0
    type: WRITE
    value_written: null
  - time: 1
    type: WRITE
    value_written: null
  - time: 2
    type: WRITE
    value_written: null"""

        test_dict_dumped = test_dict.dump_history()
        self.assertEqual(test_dict_dumped, expected_test_dict)
        final_dict = HistoryDict(yaml_string=test_dict_dumped)
        self.assertEqual(expected_test_dict, final_dict.dump_history())

    def test_make_historic(self):
        actual_dict = {"foo": "bar", "a": "b", "innerdict": {"innerkey": "innerval"}}
        test_dict = make_historic(actual_dict)
        test_dict["a"] = "c"

        # Updating actual_dict doesn't affect test_dict (it copied).
        actual_dict["foo"] = "za"
        # Similarly, updating the inner dict doesn't either.
        actual_dict["innerdict"]["innerkey"] = "innerval2"

        # However, updating the inner dict on the test_dict does.
        test_dict["innerdict"]["innerkey"] = "secondinnerval"
        expected_test_dict = """SchemaVersion: "0.1"


History:


  a:
  - time: 1
    type: WRITE
    value_written: b
  - time: 3
    type: WRITE
    value_written: c


  foo:
  - time: 0
    type: WRITE
    value_written: bar


  innerdict:
  - time: 2
    type: WRITE
    value_written: null
  - time: 4
    type: WRITE
    value_written: null"""

        self.assertEqual(test_dict.dump_history(), expected_test_dict)

    def test_dump_and_load(self):
        test_dict = HistoryDict()

        test_dict["key1"] = "key1value1"
        test_dict["key1"] = "key1value2"
        test_dict["key2"] = "key2value1"
        del test_dict["key1"]

        # Testing with location would be flaky across machines since it
        # uses absolute pathing. It's just for human convenience anyway.
        expected_test_dict = """SchemaVersion: "0.1"


History:


  key1:
  - time: 0
    type: WRITE
    value_written: key1value1
  - time: 1
    type: WRITE
    value_written: key1value2
  - time: 3
    type: DELETE
    value_written: null


  key2:
  - time: 2
    type: WRITE
    value_written: key2value1"""

        self.assertEqual(test_dict.dump_history(), expected_test_dict)

        test_dict["key2"] = "key2value2"
        second_dict = HistoryDict(yaml_string=expected_test_dict)

        self.assertRaises(KeyError, lambda: second_dict["key1"])
        self.assertEqual(second_dict["key2"], "key2value1")

        # Include the reads / writes we just did.
        expected_second_dict = """SchemaVersion: "0.1"


History:


  key1:
  - time: 0
    type: WRITE
    value_written: key1value1
  - time: 1
    type: WRITE
    value_written: key1value2
  - time: 3
    type: DELETE
    value_written: null


  key2:
  - time: 2
    type: WRITE
    value_written: key2value1"""

        self.assertEqual(second_dict.dump_history(), expected_second_dict)

    def test_write_equality(self):
        test_dict = HistoryDict()
        test_dict["foo"] = "bar"
        test_dict["myint"] = 1
        test_dict["foo"] = "za"
        test_dict["innerdict"] = make_historic({"a": "b"})

        second_dict = HistoryDict()
        second_dict["foo"] = "bar"
        second_dict["myint"] = 1
        second_dict["foo"] = "za"
        second_dict["innerdict"] = make_historic({"a": "b"})

        self.assertTrue(test_dict.write_equals(second_dict))

        second_dict["another"] = "write"
        self.assertFalse(test_dict.write_equals(second_dict))
        test_dict["another"] = "write"
        self.assertTrue(test_dict.write_equals(second_dict))

        # Reads aren't counted
        gotten_value = second_dict["foo"]  # pylint: disable=unused-variable
        self.assertTrue(test_dict.write_equals(second_dict))
