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
from wtscenario import make_scenarios

# test_dump03.py
#     Test 'wt dump' window functionality.
class test_dump(wttest.WiredTigerTestCase, suite_subprocess):
    table_format = 'key_format=u,value_format=u'
    uri = 'table:test_dump'
    output = 'dump.out'
    data_header = 'Data\n'

    n_rows = 100

    format_values = [
        ('window-size-1', dict(key='key2', winsize=1, expected_num_lines=3*2)),
        ('window-size-1-at-start', dict(key='key1', winsize=1, expected_num_lines=2*2)),
        ('window-size-1-at-end', dict(key='key99', winsize=1, expected_num_lines=2*2)),
        ('window-size-2', dict(key='key3', winsize=2, expected_num_lines=5*2)),
        ('window-size-3', dict(key='key5', winsize=3, expected_num_lines=7*2)),
        ('window-size-0', dict(key='key61', winsize=0, expected_num_lines=1*2))
    ]
    scenarios = make_scenarios(format_values)
    
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

    def test_window(self):
        self.session.create(self.uri, self.table_format)
        self.populate_table(self.uri, self.n_rows)

        
        self.runWt(['dump', '-k', self.key, '-w', str(self.winsize), self.uri], outfilename=self.output)
        num_lines = self.get_num_data_lines_from_dump(self.output)
        assert num_lines == self.expected_num_lines, num_lines


if __name__ == '__main__':
    wttest.run()
