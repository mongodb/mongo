"""Unit tests for the selected_tests script."""

import unittest

from buildscripts import errorcodes

# Debugging
errorcodes.list_files = True

TESTDATA_DIR = "./buildscripts/tests/data/errorcodes/"


class TestErrorcodes(unittest.TestCase):
    """Test errorcodes.py."""

    def setUp(self):
        # errorcodes.py keeps some global state.
        errorcodes.codes = []

    def test_regex_matching(self):
        """Test regex matching."""
        captured_error_codes = []

        def accumulate_files(code):
            captured_error_codes.append(code)

        errorcodes.parse_source_files(accumulate_files, TESTDATA_DIR + "regex_matching/")
        self.assertEqual(32, len(captured_error_codes))

    def test_dup_checking(self):
        """Test dup checking."""
        assertions, errors, _ = errorcodes.read_error_codes(TESTDATA_DIR + "dup_checking/")
        # `assertions` is every use of an error code. Duplicates are included.
        self.assertEqual(4, len(assertions))
        self.assertEqual([1, 2, 3, 2], list(map(lambda x: int(x.code), assertions)))

        # All assertions with the same error code are considered `errors`.
        self.assertEqual(2, len(errors))
        self.assertEqual(2, int(errors[0].code))
        self.assertEqual(2, int(errors[1].code))

    def test_generate_next_code(self):
        """Test `get_next_code`."""
        _, _, seen = errorcodes.read_error_codes(TESTDATA_DIR + "generate_next_code/")
        generator = errorcodes.get_next_code(seen)
        self.assertEqual(21, next(generator))
        self.assertEqual(22, next(generator))

    def test_generate_next_server_code(self):
        """
        Test `generate_next_server_code`.

        This call to `read_error_codes` technically has no bearing on `get_next_code` when a
        `server_ticket` is passed in. But it maybe makes sense for the test to do so in case a
        future patch changes that relationship.
        """
        _, _, seen = errorcodes.read_error_codes(TESTDATA_DIR + "generate_next_server_code/")
        print("Seen: " + str(seen))
        generator = errorcodes.get_next_code(seen, server_ticket=12301)
        self.assertEqual(1230101, next(generator))
        self.assertEqual(1230103, next(generator))

    def test_ticket_coersion(self):
        """Test `coerce_to_number`."""
        self.assertEqual(0, errorcodes.coerce_to_number(0))
        self.assertEqual(1234, errorcodes.coerce_to_number("1234"))
        self.assertEqual(1234, errorcodes.coerce_to_number("server-1234"))
        self.assertEqual(1234, errorcodes.coerce_to_number("SERVER-1234"))
        self.assertEqual(-1, errorcodes.coerce_to_number("not a ticket"))
