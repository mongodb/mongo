#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wttest
import re

# test_turtle.py
# The following test is to validate the turtle file and to ensure it
# contains the correct key-value pairs.


class test_turtle01(wttest.WiredTigerTestCase):
    uri = "table:test_turtle"
    nrows = 1000
    WT_METADATA_VERSION = "WiredTiger version"
    WT_METADATA_VERSION_FORMAT = "major=(\d+),minor=(\d+),patch=(\d+)"
    WT_METADATA_VERSION_STRING = "WiredTiger version string"
    WT_METADATA_VERSION_STRING_FORMAT = "(\d+)\.(\d+)\.(\d+)"
    WT_TURTLE_FILE_NAME = "WiredTiger.turtle"

    def init_values(self):
        self.val = "aaaa"
        self.key_format = "i"
        self.value_format = "S"

    def test_validate_turtle_file(self):

        # Validate the .turtle file for the empty database.
        self.read_turtle()
        self.check_turtle()

        # Create a table and perform some I/O.
        self.init_values()
        create_params = "key_format={},value_format={}".format(
            self.key_format, self.value_format)

        self.session.create(self.uri, create_params)
        cursor = self.session.open_cursor(self.uri)

        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[i] = self.val
        self.session.commit_transaction()
        self.session.checkpoint()

        # Validate the .turtle file for the non-empty database.
        self.read_turtle()
        self.check_turtle()
        self.check_metadata()

    
    def check_metadata(self):
        # The checkpoint metadata is contained in the last line of the turtle file.
        # Ensure that the keys are comma separated.
        metadata = self.turtle_file[-1].split(',')
        self.assertNotEqual(len(metadata), 1)

    # Verify that the format of the passed k-v pair matches what we expect.
    def find_and_check_wt_version(self, key: str, expected_regex: str):
        result = self.find_kv(key)
        match = re.search(expected_regex, result)
        self.assertTrue(match is not None)
        # Return the WT version as a 3-tuple of the form (major, minor, patch).
        return (match.group(1), match.group(2), match.group(3))

    def check_turtle(self):
        str_version = self.find_and_check_wt_version(
            self.WT_METADATA_VERSION_STRING, self.WT_METADATA_VERSION_STRING_FORMAT)
        version = self.find_and_check_wt_version(
            self.WT_METADATA_VERSION, self.WT_METADATA_VERSION_FORMAT)
        # Check the WT versions specified are equivalent to each other.
        self.assertEqual(str_version, version)

    # Return the value for the associated key in the .turtle file.
    def find_kv(self, key: str):
        for i in range(len(self.turtle_file)):
            if self.turtle_file[i] == key:
                return self.turtle_file[i+1]
        raise KeyError(
            "key '{}' is not present in the .turtle file".format(key))

    def read_turtle(self):
        with open(self.WT_TURTLE_FILE_NAME, 'r') as f:
            lines = f.read().splitlines()
            self.turtle_file = lines
