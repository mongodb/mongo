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
from suite_subprocess import suite_subprocess
import random

# test_dump05.py
# Test dump utility with -j option, validate JSON format.
class test_dump05(wttest.WiredTigerTestCase, suite_subprocess):
    output = 'dump.out'

    def wrap_in_json(self, s1, s2):
        return f"\"{s1}\" : \"{s2}\""

    # Check that the dumped records are in valid JSON format
    def validate_json_dump(self, create_params):
        uri = 'table:test_dump'
        self.session.create(uri, create_params)

        key = 'key'
        value = 'value'
        records = 1000

        # Generate records with varying key/value lengths that are identifiable if they reappear
        # as junk values later on in the dump.
        cursor = self.session.open_cursor(uri)
        for i in range(records):
            n = random.randint(1, 10)
            suffix = n * ('_' + str(i).zfill(3))
            cursor[key + suffix] = value + suffix
        cursor.close()

        # Call dump with -j option
        self.runWt(['dump', '-j', uri], outfilename=self.output)

        # Ensure correct number of quotes and no junk after final quotes
        self.check_file_not_contains(self.output, '/' + self.wrap_in_json('key0', '(\")+') + ',\n/')
        self.check_file_not_contains(self.output, '/' + self.wrap_in_json('value0', '(\")+') + '\n/')

        # Ensure valid records exist
        self.check_file_contains(self.output, '/' + self.wrap_in_json('key0', '[^\"]+') + ',\n/')
        self.check_file_contains(self.output, '/' + self.wrap_in_json('value0', '[^\"]+') + '\n/')

    # Test with strings
    def test_dump_string(self):
        self.validate_json_dump(create_params='key_format=S,value_format=S')

    # Test with bytes
    def test_dump_bytes(self):
        self.validate_json_dump(create_params='key_format=u,value_format=u')
