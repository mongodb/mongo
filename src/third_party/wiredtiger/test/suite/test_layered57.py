#!/usr/bin/env python3
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

import random, string, wttest
from wiredtiger import stat
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered57.py
#    Test that a follower never use application threads to evict pages with updates and dirty pages.
@disagg_test_class
class test_layered57(wttest.WiredTigerTestCase, DisaggConfigMixin):
    disagg_storages = gen_disagg_storages('test_layered57', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'disaggregated=(page_log=palm),cache_size=10MB,statistics=(all),disaggregated=(role="follower")'

    nitems = 1000

    def generate_random_string(self, length):
        characters = string.ascii_letters + string.digits + string.punctuation
        random_string = ''.join(random.choices(characters, k=length))
        return random_string

    def test_follower_not_do_app_evict(self):
        uri = "layered:test_layered57"

        # Setup.
        self.session.create(uri, 'key_format=S,value_format=S')

        # Insert some data.
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(1, self.nitems):
            self.session.begin_transaction()
            cursor[self.generate_random_string(1000) + str(i)] = self.generate_random_string(1000) + str(i)
            self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")

        stat_cursor = self.session.open_cursor('statistics:')
        self.assertGreater(stat_cursor[stat.conn.cache_eviction_app_threads_skip_updates_dirty_page][2], 0)
        stat_cursor.close()
