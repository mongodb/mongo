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

import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios


@disagg_test_class
class test_disagg_fast_truncate02(wttest.WiredTigerTestCase):
    """
    Check that verify accounts for fast-truncated pages that haven't been discarded yet.

    If checkpoint runs before a fast-truncate is globally visible, the parent page keeps proxy cells
    for the truncated leaf pages, and they're not discarded until the parent is reconciled.

    Once the truncate becomes globally visible, tree walks skip the deleted refs, so verify's page
    discard check must collect the page IDs from the refs' addresses or it reports pages missing
    from the btree that PALI correctly considers live.

    Verifying the tree contents instantiates deleted pages, which hides the problem. So we use a
    small cache and aggressive eviction, and hopefully the instantiated pages revert to deleted refs
    before the page-discard check walks them.

    """

    uri = "table:test_disagg_fast_truncate02"
    nrows = 20000
    value = "a" * 100
    trunc_start = 1000
    trunc_stop = 19000

    truncate_ts = 20
    visible_ts = 30

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'cache_size=50MB,statistics=(all),debug_mode=(eviction=true),disaggregated=(role="leader"),'

    def evict_keys(self, keys):
        """Evict the pages holding the given keys."""
        with (
            wttest.open_cursor(
                self.session, self.uri, config="debug=(release_evict)"
            ) as evict_cursor,
            self.transaction(rollback=True),
        ):
            for key in keys:
                evict_cursor.set_key(key)
                evict_cursor.search()
                evict_cursor.reset()

    def populate(self):
        """Create the table, load it, checkpoint, and evict all leaf pages to disk."""
        self.conn.set_timestamp("oldest_timestamp=" + self.timestamp_str(1))

        # Small pages spread data over many leaf pages, so the truncate fully covers some.
        self.session.create(
            self.uri,
            "key_format=i,value_format=S,block_manager=disagg,log=(enabled=false),"
            "allocation_size=512,leaf_page_max=512,internal_page_max=512",
        )

        with (
            wttest.open_cursor(self.session, self.uri) as cursor,
            self.transaction(commit_timestamp=10),
        ):
            for key in range(1, self.nrows + 1):
                cursor[key] = self.value

        self.conn.set_timestamp("stable_timestamp=" + self.timestamp_str(10))
        self.session.checkpoint()

        # On-disk leaf pages are eligible for fast truncate.
        self.evict_keys(range(1, self.nrows + 1))

    def fast_truncate(self, trunc_start, trunc_stop, truncate_ts):
        """Fast-truncate the configured key range at truncate_ts."""
        with (
            wttest.open_cursor(self.session, self.uri) as start_cursor,
            wttest.open_cursor(self.session, self.uri) as stop_cursor,
            self.transaction(commit_timestamp=truncate_ts),
        ):
            start_cursor.set_key(trunc_start)
            stop_cursor.set_key(trunc_stop)
            self.session.truncate(None, start_cursor, stop_cursor, None)

    def test_verify_undiscarded_fast_truncated_pages(self):
        self.populate()

        fast_delete_before = self.get_stat(stat.dsrc.rec_page_delete_fast, self.uri)

        # With oldest pinned at 1 the truncate is committed but not globally visible, so the
        # checkpoint writes proxy cells for the truncated leaf pages and their blocks aren't
        # discarded yet in PALI.
        self.fast_truncate(self.trunc_start, self.trunc_stop, self.truncate_ts)
        self.assertStatGreaterSoon(
            stat.dsrc.rec_page_delete_fast,
            fast_delete_before,
            self.uri,
            msg="fast truncate did not trigger",
        )

        self.conn.set_timestamp(
            "stable_timestamp=" + self.timestamp_str(self.truncate_ts)
        )
        self.session.checkpoint()

        # Once the truncate is globally visible, tree walks skip the deleted refs, but their blocks
        # remain allocated until the parent is reconciled. The page-discard check in verify must
        # still count these pages.
        ts = self.timestamp_str(self.visible_ts)
        self.conn.set_timestamp(f"oldest_timestamp={ts},stable_timestamp={ts}")

        self.conn.reconfigure('cache_size=2MB,debug_mode=(eviction=true)')
        self.verifyUntilSuccess(self.session, self.uri)
