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

import wttest
from helpers.helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# test_disagg_checkpoint_shared_metadata_evict.py
#   A checkpoint writes the shared metadata of every table it checkpointed, in its own transaction.
#   Writing enough of it forces eviction of the page being written, which must not discard the
#   updates the checkpoint has yet to commit.
@disagg_test_class
class test_disagg_checkpoint_shared_metadata_evict(wttest.WiredTigerTestCase):

    test_name = __qualname__

    # A small cache lowers the in-memory page size at which a page is forcibly evicted when the
    # thread writing it releases it, so a modest number of tables is enough.
    conn_base_config = 'cache_size=4MB,statistics=(all),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # A table contributes two shared metadata entries, and a checkpoint writes each of them twice:
    # once for the table's creation and once to record the checkpoint it just wrote. Enough tables
    # to take the page well past the size at which it is forcibly evicted, which the test checks.
    ntables = 100

    # Padding carried into each shared metadata entry, so the page reaches that size on a much
    # wider range of cache sizes than the entries alone would.
    padding = 'x' * 4096

    def forced_evictions(self):
        with wttest.open_cursor(self.session, 'statistics:') as stat_cursor:
            return stat_cursor[stat.conn.eviction_force][2] + \
                stat_cursor[stat.conn.eviction_force_fail][2]

    def test_shared_metadata_forced_evict(self):
        uris = [f'layered:{self.test_name}_{i}' for i in range(self.ntables)]

        for uri in uris:
            self.session.create(uri,
                f'key_format=S,value_format=S,app_metadata="{self.padding}"')
            cursor = self.session.open_cursor(uri)
            cursor['key'] = 'value'
            cursor.close()

        before = self.forced_evictions()
        self.session.checkpoint()

        # Without a forced eviction while the checkpoint was writing shared metadata, this test
        # proves nothing, so fail rather than pass silently.
        self.assertGreater(self.forced_evictions(), before,
            'no page was forcibly evicted, the configuration no longer exercises the checkpoint')

        # The connection must remain usable, and a second checkpoint must be able to write more
        # shared metadata on top of what the first one wrote.
        for uri in uris:
            cursor = self.session.open_cursor(uri)
            self.assertEqual(cursor['key'], 'value')
            cursor['key2'] = 'value2'
            cursor.close()

        self.session.checkpoint()
