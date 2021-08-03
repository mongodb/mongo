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

class test_assert03(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'log=(enabled)'
    base_uri = 'file:assert03.wt'
    cfg = 'key_format=S,value_format=S'
    always = 'assert=(commit_timestamp=always)'
    never = 'assert=(commit_timestamp=never)'
    none = 'assert=(commit_timestamp=none)'

    def test_assert03(self):
        #if not wiredtiger.diagnostic_build():
        #    self.skipTest('requires a diagnostic build')

        # Create a data item at the default setting
        self.session.create(self.base_uri, self.cfg)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c['key0'] = 'value0'
        self.session.commit_transaction()
        c.close()

        # Now rotate through the alter settings and verify the data.
        # The always setting should fail.
        self.session.alter(self.base_uri, self.always)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c['key1'] = 'value1'
        msg = "/none set on this transaction/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.commit_transaction(), 0), msg)
        c.close()

        # The never and none settings should succeed.
        self.session.alter(self.base_uri, self.never)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c['key2'] = 'value2'
        self.session.commit_transaction()
        c.close()

        self.session.alter(self.base_uri, self.none)
        c = self.session.open_cursor(self.base_uri)
        self.session.begin_transaction()
        c['key3'] = 'value3'
        self.session.commit_transaction()
        c.close()

if __name__ == '__main__':
    wttest.run()
