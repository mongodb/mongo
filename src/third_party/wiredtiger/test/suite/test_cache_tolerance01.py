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
# Test that cache_tolerance_for_app_eviction applies without incremental application eviction.
#

from contextlib import closing
import wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios


class test_cache_tolerance01(wttest.WiredTigerTestCase):
    """A reader stays out of eviction while the dirty content is within the cache tolerance above
    the dirty trigger, unless the cache is over the eviction trigger."""

    uri = f"file:{__qualname__}"
    probe_uri = f"file:{__qualname__}_probe"

    # A tolerance of 100 keeps all readers out of eviction between 20% and 24% dirty.
    dirty_pct_goal = 22

    scenarios = make_scenarios([
        ("no_tolerance", dict(tolerance=0, eviction_trigger=95, assist=True)),
        ("within_tolerance", dict(tolerance=100, eviction_trigger=95, assist=False)),
        ("over_eviction_trigger", dict(tolerance=100, eviction_trigger=20, assist=True)),
    ])

    def conn_config(self):
        # The updates trigger is set high so that the test exercises the dirty trigger.
        return ("cache_size=200MB,statistics=(all),eviction_target=10,"
            f"eviction_trigger={self.eviction_trigger},eviction_dirty_trigger=20,"
            "eviction_updates_trigger=90,"
            f"eviction=(threads_max=1,cache_tolerance_for_app_eviction={self.tolerance})")

    def dirty_pct(self, session):
        stats = self.get_stats(
            [stat.conn.cache_bytes_dirty_leaf, stat.conn.cache_bytes_max], "", session)
        return 100.0 * stats[stat.conn.cache_bytes_dirty_leaf] / stats[stat.conn.cache_bytes_max]

    def test_cache_tolerance(self):
        # Eviction must not change the dirty content while the test measures it.
        self.session.create(self.uri, "key_format=i,value_format=S,cache_resident=true")
        self.session.create(self.probe_uri, "key_format=i,value_format=S")
        with wttest.open_cursor(self.session, self.probe_uri) as cursor:
            cursor[1] = "probe"

        # An uncommitted transaction keeps the dirty content above the dirty trigger. The writer
        # ignores the cache size so it is not pulled into eviction.
        with closing(self.conn.open_session("ignore_cache_size=true")) as pin:
            pin.begin_transaction()
            with wttest.open_cursor(pin, self.uri) as cursor:
                key = 0
                while self.dirty_pct(pin) < self.dirty_pct_goal:
                    for _ in range(100):
                        key += 1
                        cursor[key] = "a" * 1000
            self.assertLess(self.dirty_pct(pin), self.dirty_pct_goal + 1)

            # A reader pulled into eviction times out and its transaction rolls back.
            # Read-committed isolation makes the dirty trigger apply to the reader.
            ops = self.get_stat(stat.conn.application_cache_ops, session=pin)
            with (
                closing(self.conn.open_session("cache_max_wait_ms=500")) as probe,
                wttest.open_cursor(probe, self.probe_uri) as cursor,
            ):
                probe.begin_transaction("isolation=read-committed")
                cursor.set_key(1)
                try:
                    cursor.search()
                except wiredtiger.WiredTigerRollbackError:
                    self.ignoreStdoutPatternIfExists("Cache capacity has overflown")
                probe.rollback_transaction()
            ops_after = self.get_stat(stat.conn.application_cache_ops, session=pin)

        if self.assist:
            self.assertGreater(ops_after, ops)
        else:
            self.assertEqual(ops_after, ops)


if __name__ == "__main__":
    wttest.run()
