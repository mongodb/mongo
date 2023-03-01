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

from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from rollback_to_stable_util import test_rollback_to_stable_base

# test_rollback_to_stable41.py
# Test that the dry-run config for RTS only applies to a single call.
class test_rollback_to_stable41(test_rollback_to_stable_base):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        return 'verbose=(rts:5)'

    def test_rollback_to_stable(self):
        uri = 'table:test_rollback_to_stable41'
        nrows = 1000

        if self.value_format == '8t':
            value_a = 97
            value_b = 98
        else:
            value_a = 'a' * 10
            value_b = 'b' * 10

        # Create our table.
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        # Insert some data either side of the stable timestamp we set below.
        self.large_updates(uri, value_a, ds, nrows, False, 10)
        self.check(value_a, uri, nrows, None, 10)
        self.large_updates(uri, value_b, ds, nrows, False, 30)
        self.check(value_b, uri, nrows, None, 30)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))

        # Fake RTS, newer data should still exist.
        self.conn.rollback_to_stable('dryrun=true')
        self.check(value_b, uri, nrows, None, 30)

        # Real RTS, newer data should vanish.
        self.conn.rollback_to_stable()
        self.check(value_a, uri, nrows, None, 30)

if __name__ == '__main__':
    wttest.run()
