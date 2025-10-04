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

import json
import wttest
from suite_subprocess import suite_subprocess

# test_dump04.py
# Test dump utility with -j and -k options
class test_dump04(wttest.WiredTigerTestCase, suite_subprocess):
    output = "dump.out"
    uri = "table:test_dump"
    create_params = "key_format=u,value_format=u"
    dict = {
        "key" : "value",
        "key0" : "value0",
        "1" : "1",
    }

    def wrap_in_json(self, s1, s2):
        return f"\"{s1}\" : \"{s2}\""

    def string_to_unicode(self, s):
        ret = ""
        for c in s:
            ret += "\\u{:04x}".format(ord(c))
        return ret

    def check_key_value(self, should_exist, key, value):
        if should_exist:
            self.check_file_contains(self.output, key)
            self.check_file_contains(self.output, value)
        else:
            self.check_file_not_contains(self.output, key)
            self.check_file_not_contains(self.output, value)

    def format_string(self, json, is_key, s):
        if json:
            return self.wrap_in_json("key0" if is_key else "value0", self.string_to_unicode(s))
        return f"{s}\n"

    def check_file(self, json, key):
        for (k, v) in self.dict.items():
            self.check_key_value(not key or k == key, self.format_string(json, True, k), self.format_string(json, False, v))

    # Check that the output file contains valid json by attempting to load it into a dictionary.
    # This will throw an exception if the json file is invalid.
    def check_valid_jason(self):
        with open(self.output, 'r') as file:
            data = json.load(file)

    def run_test(self, json, key):
        args = ["dump"]
        if json:
            args.append("-j")
        if key:
            args += ["-k", key]
        args.append(self.uri)
        self.runWt(args, outfilename=self.output)

        self.check_file(json, key)

        if json:
            self.check_valid_jason()

    def test_dump(self):
        self.session.create(self.uri, self.create_params)
        cursor = self.session.open_cursor(self.uri)
        for (k, v) in self.dict.items():
            cursor[k] = v
        cursor.close()

        # Perform checkpoint, to clean the dirty pages and place values on disk.
        self.session.checkpoint()

        self.run_test(True, "") # Dump command with -j option
        self.run_test(False, "key") # Dump command with -k option matching key
        self.run_test(False, "table") # Dump command with -k option non matching key
        self.run_test(True, "1") # Dump command with -j -k options matching key
        self.run_test(True, "table") # Dump command with -j -k options non matching key

