"""Unit tests for the evergreen_task_timeout script."""
import unittest

from buildscripts.validate_commit_message import STATUS_ERROR, STATUS_OK, main


class ValidateCommitMessageTest(unittest.TestCase):
    def test_valid(self):
        messages = [
            "SERVER-44338",
            "Revert \"SERVER-60",
            "Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4",
            'Revert "Import wiredtiger: 58115abb6fbb3c1cc7bfd087d41a47347bce9a69 from branch mongodb-4.4"',
        ]

        self.assertTrue(all(main([message]) == STATUS_OK for message in messages))

    def test_invalid(self):
        messages = [
            "SERVER-",  # missing number
            "Revert SERVER-60",  # missing quote before SERVER
            "",  # empty value
            "nonsense",  # nonsense value
        ]

        self.assertTrue(all(main([message]) == STATUS_ERROR for message in messages))

    def test_message_is_empty_list(self):
        self.assertEqual(main([]), STATUS_ERROR)
