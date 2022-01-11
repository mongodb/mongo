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
# test_assert02.py
#   Timestamps: assert read timestamp settings
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_assert02(wttest.WiredTigerTestCase, suite_subprocess):

    key_format_values = [
        ('column', dict(key_format='r', usestrings=False)),
        ('string-row', dict(key_format='S', usestrings=True))
    ]
    scenarios = make_scenarios(key_format_values)

    def test_read_timestamp(self):
        #if not wiredtiger.diagnostic_build():
        #    self.skipTest('requires a diagnostic build')

        base = 'assert02.'
        base_uri = 'file:' + base
        uri_always = base_uri + '.always.wt'
        uri_def = base_uri + '.def.wt'
        uri_never = base_uri + '.never.wt'
        uri_none = base_uri + '.none.wt'

        cfg = 'key_format={},value_format=S'.format(self.key_format)
        cfg_always = cfg + ',write_timestamp_usage=always,assert=(read_timestamp=always)'
        cfg_def = cfg
        cfg_never = cfg + ',assert=(read_timestamp=never)'
        cfg_none = cfg + ',assert=(read_timestamp=none)'

        # Create a data item at a timestamp.
        self.session.create(uri_always, cfg_always)
        self.session.create(uri_def, cfg_def)
        self.session.create(uri_never, cfg_never)
        self.session.create(uri_none, cfg_none)

        # Make a key.
        key1 = 'key1' if self.usestrings else 1

        # Insert a data item at timestamp 1.  This should work for all.
        c_always = self.session.open_cursor(uri_always)
        c_def = self.session.open_cursor(uri_def)
        c_never = self.session.open_cursor(uri_never)
        c_none = self.session.open_cursor(uri_none)
        self.session.begin_transaction()
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(1))
        c_always[key1] = 'value1'
        c_def[key1] = 'value1'
        c_never[key1] = 'value1'
        c_none[key1] = 'value1'
        self.session.commit_transaction()
        c_always.close()
        c_def.close()
        c_never.close()
        c_none.close()

        # Now that we have a timestamped data, try reading with and without
        # the timestamp.
        c_always = self.session.open_cursor(uri_always)
        c_def = self.session.open_cursor(uri_def)
        c_never = self.session.open_cursor(uri_never)
        c_none = self.session.open_cursor(uri_none)

        c_always.set_key(key1)
        c_def.set_key(key1)
        c_never.set_key(key1)
        c_none.set_key(key1)

        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(1))
        c_always.search()
        c_def.search()
        c_none.search()
        self.assertEqual(c_always.get_value(), 'value1')
        self.assertEqual(c_def.get_value(), 'value1')
        self.assertEqual(c_none.get_value(), 'value1')

        msg = "/timestamp set on this transaction/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(c_never.search(), 0), msg)
        self.session.rollback_transaction()
        c_always.close()
        c_def.close()
        c_never.close()
        c_none.close()

        # Read in a transaction without a timestamp.
        c_always = self.session.open_cursor(uri_always)
        c_def = self.session.open_cursor(uri_def)
        c_never = self.session.open_cursor(uri_never)
        c_none = self.session.open_cursor(uri_none)

        c_always.set_key(key1)
        c_def.set_key(key1)
        c_never.set_key(key1)
        c_none.set_key(key1)

        self.session.begin_transaction()
        c_never.search()
        c_def.search()
        c_none.search()
        self.assertEqual(c_never.get_value(), 'value1')
        self.assertEqual(c_def.get_value(), 'value1')
        self.assertEqual(c_none.get_value(), 'value1')

        msg = "/none set on this transaction/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(c_always.search(), 0), msg)
        self.session.rollback_transaction()
        c_always.close()
        c_def.close()
        c_never.close()
        c_none.close()

if __name__ == '__main__':
    wttest.run()
