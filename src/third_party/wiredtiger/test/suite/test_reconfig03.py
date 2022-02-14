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

import time
import wttest
from wtdataset import SimpleDataSet

# test_reconfig03.py
#    Test the connection reconfiguration operations used in the MongoDB
#    test reconfigwt.js.
class test_reconfig03(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled,file_max=100K,prealloc=false,remove=false,zero_fill=false),checkpoint=(wait=1),cache_size=1G'
    uri = "table:reconfig03"

    # Reconfigure similar to MongoDB tests.  Sleep so that checkpoint
    # can run after we've made modifications.
    def test_reconfig03_mdb(self):
        entries = 10000
        SimpleDataSet(self, self.uri, entries).populate()
        time.sleep(1)
        self.conn.reconfigure("eviction_target=81")
        SimpleDataSet(self, self.uri, entries * 2).populate()
        time.sleep(1)
        self.conn.reconfigure("cache_size=81M")
        SimpleDataSet(self, self.uri, entries * 3).populate()
        time.sleep(1)
        self.conn.reconfigure("eviction_dirty_target=82")
        SimpleDataSet(self, self.uri, entries * 4).populate()
        time.sleep(1)
        self.conn.reconfigure("shared_cache=(chunk=11MB, name=bar, reserve=12MB, size=1G)")

    def test_reconfig03_log_size(self):
        #
        # Reconfigure checkpoint based on log size.
        #
        self.conn.reconfigure("checkpoint=(log_size=20)")
        self.conn.reconfigure("checkpoint=(log_size=1M)")
        self.conn.reconfigure("checkpoint=(log_size=0)")

if __name__ == '__main__':
    wttest.run()
