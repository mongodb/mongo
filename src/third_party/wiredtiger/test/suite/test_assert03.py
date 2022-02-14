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
# test_assert03.py
# Test changing assert setting via alter.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_assert03(wttest.WiredTigerTestCase, suite_subprocess):
    base_uri = 'file:assert03.wt'
    always = 'write_timestamp_usage=always,assert=(write_timestamp=on)'
    never = 'write_timestamp_usage=never,assert=(write_timestamp=on)'
    none = 'assert=(write_timestamp=off)'

    key_format_values = [
        ('col-fix', dict(key_format='r', value_format='8t')),
        ('col', dict(key_format='r', value_format='S')),
        ('row', dict(key_format='S', value_format='S'))
    ]
    scenarios = make_scenarios(key_format_values)

    def test_assert03(self):
        #if not wiredtiger.diagnostic_build():
        #    self.skipTest('requires a diagnostic build')

        cfg = 'key_format={},'.format(self.key_format) + 'value_format={}'.format(self.value_format)
        key0 = 'key0' if self.key_format == 'S' else 17
        value0 = 'value0' if self.value_format == 'S' else 0x2a
        key1 = 'key1' if self.key_format == 'S' else 18
        value1 = 'value1' if self.value_format == 'S' else 0x2b
        key2 = 'key2' if self.key_format == 'S' else 19
        value2 = 'value2' if self.value_format == 'S' else 0x2c
        key3 = 'key3' if self.key_format == 'S' else 20
        value3 = 'value3' if self.value_format == 'S' else 0x2d

        # Create a data item at the default setting
        self.session.create(self.base_uri, cfg)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c[key0] = value0
        self.session.commit_transaction()
        c.close()

        # Now rotate through the alter settings and verify the data.
        # The always setting should fail.
        self.session.alter(self.base_uri, self.always)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c[key1] = value1
        msg = "/none set on this transaction/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.commit_transaction(), 0), msg)
        c.close()

        # The never and none settings should succeed.
        self.session.alter(self.base_uri, self.never)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c[key2] = value2
        self.session.commit_transaction()
        c.close()

        self.session.alter(self.base_uri, self.none)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c[key3] = value3
        self.session.commit_transaction()
        c.close()

if __name__ == '__main__':
    wttest.run()
