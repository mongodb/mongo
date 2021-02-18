#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

import wiredtiger, wtscenario, wttest
from wiredtiger import stat

# test_tiered05.py
#    Basic tiered storage API test.
class test_tiered05(wttest.WiredTigerTestCase):
    uri = "table:test_tiered05"

    auth_token = "test_token"
    extension_name = "test"
    def conn_config(self):
        return \
          'statistics=(fast),' + \
          'tiered_manager=(wait=10),' + \
          'tiered_storage=(enabled,object_target_size=20M,' + \
          'name=%s,' % self.extension_name + \
          'auth_token=%s)' % self.auth_token

    # Test calling the flush_tier API.
    def test_tiered(self):
        self.session.create(self.uri, 'key_format=S')
        msg = "/storage manager thread is configured/"
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.assertEquals(self.session.flush_tier(None), 0), msg)

if __name__ == '__main__':
    wttest.run()
