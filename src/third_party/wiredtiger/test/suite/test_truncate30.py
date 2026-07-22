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
# checkpoint:recovery
# truncate
# [END_TAGS]

import wiredtiger
import wttest
from helper import simulate_crash_restart

class test_truncate30(wttest.WiredTigerTestCase):
    uri = 'table:truncate30'

    # Very small pages create a deep tree with many internal pages.
    # More internal pages  more splits during eviction  higher probability
    # that at least one dsk==NULL page ends up holding page_del children.
    create_config = (
        'key_format=i,value_format=S,'
        'allocation_size=512,leaf_page_max=512,internal_page_max=512'
    )

    nrows   = 50000
    trunc_lo = 20000
    trunc_hi = 30000

    # Logging must be enabled so that Phase 2 operations (truncation + mass
    # updates) are written to the WAL.  simulate_crash_restart captures the WAL
    # by copying files while the connection is open; recovery in the copy then
    # replays those operations, which triggers the leaf/internal page splits
    # that create the dsk==NULL pages we need to exercise the fix.
    #
    # cache_size=1MB (the WiredTiger minimum). With ~1 282 dirty leaf pages
    # each expanding from 3-char to 15-char values (~1.3 KB per page in memory),
    # total in-memory data (~1.7 MB) exceeds the 1 MB budget, forcing eviction
    # to run during recovery and reconcile/split those leaves.
    conn_config = 'cache_size=1MB,log=(enabled=true),debug_mode=(eviction=true),timing_stress_for_test=[split_3,failpoint_eviction_split]'

    def test_truncate_recovery_null_dsk(self):
        # ---- Phase 1: baseline data on disk ----
        self.session.create(self.uri, self.create_config)
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            cursor[i] = 'vvv'
        cursor.close()
        self.session.checkpoint()

        # ---- Phase 2: operations written to WAL only (no checkpoint) ----
        #
        # Truncation first so that during recovery, when the subsequent updates
        # cause leaf splits and those splits propagate to level-2 internal page
        # splits, the page_del from the truncation is already present on the
        # refs inside those level-2 pages.
        lo = self.session.open_cursor(self.uri)
        hi = self.session.open_cursor(self.uri)
        lo.set_key(self.trunc_lo)
        hi.set_key(self.trunc_hi)
        self.session.truncate(None, lo, hi, None)
        lo.close()
        hi.close()

        # Update every non-truncated key with a 15-char value. Each leaf page
        # (512 B, ~39 keys at 3 chars) will reconstitute to ~1170 B during
        # eviction, forcing a split into 2-3 new pages. With ~1282 dirty leaf
        # pages and a 1 MB cache, eviction is aggressive throughout recovery.
        val = 'x' * 100
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.trunc_lo):
            cursor[i] = val
        for i in range(self.trunc_hi + 1, self.nrows + 1):
            cursor[i] = val
        cursor.close()
        # Intentionally no checkpoint here all of Phase 2 is WAL-only

        # ---- Phase 3: crash-restart ----
        # simulate_crash_restart copies files while the connection is still open
        # (capturing the WAL), then closes the original and opens the copy.
        # Recovery replays Phase 2. Under cache pressure, leaf splits cascade
        # into level-2 internal page splits that create dsk==NULL pages
        # inheriting page_del children from the replayed truncation.
        # Without the fix: SIGSEGV. With the fix: recovery completes cleanly.
        #
        # The WAL is copied while it may be mid-write, so recovery can find a torn
        # final record and salvage/truncate the log. Those NOTICE messages are an
        # expected outcome of the crash simulation, not a failure.
        self.ignoreStdoutPattern('log record at position|corrupted at position|salvage: log file')
        simulate_crash_restart(self, '.', 'CRASH')

        # ---- Phase 4: verify integrity ----
        # Spot-check data outside the truncated range.
        cursor = self.session.open_cursor(self.uri)
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)
        cursor.set_key(self.trunc_lo - 1)
        self.assertEqual(cursor.search(), 0)
        cursor.set_key(self.nrows)
        self.assertEqual(cursor.search(), 0)

        # Spot-check that truncated keys are gone.
        cursor.set_key((self.trunc_lo + self.trunc_hi) // 2)
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()
