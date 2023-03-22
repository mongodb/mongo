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
#
# [TEST_TAGS]
# wt_util
# [END_TAGS]

import wttest
from suite_subprocess import suite_subprocess

# test_dump02.py
# Test the dump utility to find different keys.
class test_dump(wttest.WiredTigerTestCase, suite_subprocess):
    table_format = 'key_format=u,value_format=u'
    uri = 'table:test_dump'
    output = 'dump.out'
    data_header = 'Data\n'
    # First line is the key, second line is the value.
    lines_per_record = 2
    n_rows = 100

    def gen_key(self, i):
        return 'key' + str(i)

    def gen_value(self, i):
        return 'value' + str(i)
    
    def get_num_data_lines_from_dump(self, file):
        """
        Get the number of lines corresponding to data from a dump file.
        """
        lines = open(file).readlines()
        data_start = lines.index(self.data_header)
        assert self.data_header not in lines[data_start + 1:]
        return len(lines) - data_start - 1

    def populate_table(self, uri, n_rows):
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, n_rows):
            cursor[self.gen_key(i)] = self.gen_value(i)
        cursor.close()

    def test_dump(self):
        self.session.create(self.uri, self.table_format)
        self.populate_table(self.uri, self.n_rows)

        self.runWt(['dump', self.uri], outfilename=self.output)
        assert self.get_num_data_lines_from_dump(self.output) == (self.n_rows - 1) * self.lines_per_record

    def test_dump_single_key(self):
        self.session.create(self.uri, self.table_format)
        self.populate_table(self.uri, self.n_rows)

        # The key does not exist.
        self.runWt(['dump', '-k', 'key0', self.uri], outfilename=self.output)
        num_lines = self.get_num_data_lines_from_dump(self.output)
        assert num_lines == 0, num_lines

        # The nearest key should be found.
        self.runWt(['dump', '-k', 'key0', '-n', self.uri], outfilename=self.output)
        num_lines = self.get_num_data_lines_from_dump(self.output)
        assert num_lines == self.lines_per_record, num_lines

        # Existing key.
        self.runWt(['dump', '-k', 'key1', self.uri], outfilename=self.output)
        num_lines = self.get_num_data_lines_from_dump(self.output)
        assert num_lines == self.lines_per_record, num_lines

    def test_dump_bounds(self):
        self.session.create(self.uri, self.table_format)
        self.populate_table(self.uri, self.n_rows)

        # Expect half of the keys: 50 to 99, as well as the keys 6, 7, 8 and 9.
        self.runWt(['dump', '-l', 'key50', self.uri], outfilename=self.output)
        num_lines = self.get_num_data_lines_from_dump(self.output)
        assert num_lines == (self.n_rows / 2) * self.lines_per_record + (4 * self.lines_per_record), num_lines

        # Expect 3 keys.
        self.runWt(['dump', '-u', 'key11', self.uri], outfilename=self.output)
        num_lines = self.get_num_data_lines_from_dump(self.output)
        assert num_lines == 3 * self.lines_per_record, num_lines

        # Expect 10 keys.
        self.runWt(['dump', '-l', 'key50', '-u', 'key59', self.uri], outfilename=self.output)
        num_lines = self.get_num_data_lines_from_dump(self.output)
        assert num_lines == 10 * self.lines_per_record, num_lines

if __name__ == '__main__':
    wttest.run()
