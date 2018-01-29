#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
# test_assert01.py
#   Timestamps: assert commit settings
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest

def timestamp_str(t):
    return '%x' % t

class test_assert01(wttest.WiredTigerTestCase, suite_subprocess):
    base = 'assert01'
    base_uri = 'file:' + base
    uri_always = base_uri + '.always.wt'
    uri_def = base_uri + '.def.wt'
    uri_never = base_uri + '.never.wt'
    uri_none = base_uri + '.none.wt'
    cfg = 'key_format=S,value_format=S,'
    cfg_always = 'assert=(commit_timestamp=always)'
    cfg_def = ''
    cfg_never = 'assert=(commit_timestamp=never)'
    cfg_none = 'assert=(commit_timestamp=none)'

    count = 1
    #
    # Commit a k/v pair making sure that it detects an error if needed, when
    # used with and without a commit timestamp.
    #
    def insert_check(self, uri, use_ts):
        c = self.session.open_cursor(uri)
        key = 'key' + str(self.count)
        val = 'value' + str(self.count)

        # Commit with a timestamp
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(self.count))
        c[key] = val
        # All settings other than never should commit successfully
        if (use_ts != 'never'):
            self.session.commit_transaction()
        else:
            msg = "/timestamp set on this transaction/"
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:self.assertEquals(self.session.commit_transaction(),
                0), msg)
        c.close()
        self.count += 1

        # Commit without a timestamp
        key = 'key' + str(self.count)
        val = 'value' + str(self.count)
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key] = val
        # All settings other than always should commit successfully
        if (use_ts != 'always'):
            self.session.commit_transaction()
        else:
            msg = "/none set on this transaction/"
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda:self.assertEquals(self.session.commit_transaction(),
                0), msg)
        self.count += 1
        c.close()

    def test_commit_timestamp(self):
        #if not wiredtiger.timestamp_build() or not wiredtiger.diagnostic_build():
        #    self.skipTest('requires a timestamp and diagnostic build')
        if not wiredtiger.timestamp_build():
            self.skipTest('requires a timestamp build')

        # Create a data item at a timestamp
        self.session.create(self.uri_always, self.cfg + self.cfg_always)
        self.session.create(self.uri_def, self.cfg + self.cfg_def)
        self.session.create(self.uri_never, self.cfg + self.cfg_never)
        self.session.create(self.uri_none, self.cfg + self.cfg_none)

        # Check inserting into each table
        self.insert_check(self.uri_always, 'always')
        self.insert_check(self.uri_def, 'none')
        self.insert_check(self.uri_never, 'never')
        self.insert_check(self.uri_none, 'none')

if __name__ == '__main__':
    wttest.run()
