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
# session_api:reconfigure
# [END_TAGS]

import wttest

# test_reconfig05.py
#    Test WT_SESSION::reconfigure
class test_reconfig05(wttest.WiredTigerTestCase):

    # Test whether the reconfiguration can handle structs without the "=" separator.
    conn_config = 'log=(enabled)'
    create_session_config = 'key_format=S,value_format=S,'
    uri = "table:reconfig05"

    def test_reconfig05(self):
        self.session.create(self.uri, self.create_session_config)

        # Check whether the following calls succeed.
        self.conn.reconfigure("cache_size=1GB")
        self.conn.reconfigure("cache_size=1GB,log=(os_cache_dirty_pct=30)")
        self.conn.reconfigure("log=(os_cache_dirty_pct=50)")
