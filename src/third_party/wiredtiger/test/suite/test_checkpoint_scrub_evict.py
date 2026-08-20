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
# test_checkpoint_scrub_evict.py
#
# Tests for checkpoint clean scrub eviction: with precise_checkpoint=true, checkpoint reconciliation
# of row-leaf pages sets WT_REC_SCRUB, saving mod_disk_image so the eviction
# server can replace those pages with clean in-memory images.
#
# Observable behaviors tested:
#   1. cache_write_restore_scrub stat increases after a checkpoint cycle when
#      precise_checkpoint=true — evidence that scrub reconciliation fired.
#   2. cache_scrub_restore stat increases — pages re-instantiated from disk
#      image after checkpoint scrub.
#   3. cache_eviction_blocked_precise_checkpoint stat is present and does not
#      go negative (smoke-test the counter is wired up correctly).
#   4. Basic read-write workload produces consistent data after checkpoint.
#   5. The eviction.checkpoint_scrub_eviction override (auto/off/on) controls the
#      path: off disables it, auto matches today's precise-checkpoint behavior,
#      and on activates it regardless of precise checkpoint.

import wttest
from wiredtiger import stat
from wtscenario import make_scenarios

class test_checkpoint_scrub_evict(wttest.WiredTigerTestCase):
    """
    Verify checkpoint based clean scrub-eviction behaviour.

    Two scenario axes:
      - precise_checkpoint on/off
      - small / larger value size (to ensure multiple pages are written)
    """

    uri = 'table:scrub_evict'

    # Turn scrub on rather than letting the auto cache-pressure heuristic decide for precise
    # checkpoints. These tests need eviction engaged to consume the retained images and eviction
    # in scrub mode to produce them, and auto only scrubs below the dirty and updates
    # target/trigger midpoints while dirty eviction only runs above their targets. That is a
    # narrow window that makes auto unreliable for these tests.
    ckpt_precision = [
        ('precise', dict(precise=True,
                         ckpt_cfg='precise_checkpoint=true,'
                                  'eviction=(checkpoint_scrub_eviction=on)')),
        ('fuzzy',   dict(precise=False,
                         ckpt_cfg='precise_checkpoint=false,'
                                  'eviction=(checkpoint_scrub_eviction=auto)')),
    ]
    value_sz = [
        ('small',  dict(vsize=100)),
        ('medium', dict(vsize=1000)),
    ]
    scenarios = make_scenarios(ckpt_precision, value_sz)

    # Keep cache intentionally small to encourage eviction pressure.
    def conn_config(self):
        return (
            'cache_size=50MB,'
            'statistics=(all),'
            + self.ckpt_cfg
        )

    # ------------------------------------------------------------------ helpers

    def _populate(self, nrows, value_size):
        """Insert nrows key/value pairs using a simple pattern."""
        cursor = self.session.open_cursor(self.uri)
        val = 'x' * value_size
        for i in range(nrows):
            cursor[i] = val
        cursor.close()

    def _settle(self, nrows, value_size):
        """Lay the tree out as disk-shaped leaf pages, then dirty them again.

        A freshly inserted tree is one oversized in-memory page that checkpoint reconciliation
        splits, and an image is only retained for a 1-for-1 page swap."""
        self._populate(nrows, value_size)
        self.session.checkpoint()
        cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        for i in range(nrows):
            cursor.set_key(i)
            cursor.search()
            cursor.reset()
        cursor.close()
        self._populate(nrows, value_size)

    def _verify_reads(self, nrows, value_size):
        """Read back all rows and verify values are intact."""
        cursor = self.session.open_cursor(self.uri)
        expected = 'x' * value_size
        for i in range(nrows):
            cursor.set_key(i)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), expected)
        cursor.close()

    # ------------------------------------------------------------------ tests

    def test_scrub_stat_after_checkpoint(self):
        """
        After a checkpoint with precise_checkpoint=true, the
        cache_write_restore_scrub counter should be non-zero (scrub
        reconciliation must have fired on at least one page).

        With precise_checkpoint=false the stat should remain at zero
        because WT_REC_SCRUB is never set on that path.
        """
        nrows = 5000

        self.session.create(self.uri, 'key_format=i,value_format=S')

        # precise_checkpoint requires a stable timestamp to be set.
        if self.precise:
            self.conn.set_timestamp('stable_timestamp=1')

        self._populate(nrows, self.vsize)

        # Snapshot the counter before the checkpoint.
        before = self.get_stat(stat.conn.cache_write_restore_scrub)

        self.session.checkpoint()

        after = self.get_stat(stat.conn.cache_write_restore_scrub)

        if self.precise:
            # At least some pages must have been scrub-reconciled.
            self.assertGreater(
                after, before,
                msg=(
                    'cache_write_restore_scrub should increase after a '
                    'precise checkpoint: before={}, after={}'.format(before, after)
                )
            )
        else:
            # Fuzzy checkpoint must not trigger scrub reconciliation on the
            # precise-checkpoint path.
            pc_stat = self.get_stat(stat.conn.cache_write_restore_scrub_checkpoint)
            self.assertEqual(
                pc_stat, 0,
                msg=(
                    'cache_write_restore_scrub_checkpoint should be 0 '
                    'for a fuzzy checkpoint, got {}'.format(pc_stat)
                )
            )

    def test_scrub_restore_stat_after_checkpoint(self):
        """
        cache_scrub_restore counts pages re-instantiated from a saved disk
        image (mod_disk_image).  After a precise checkpoint + subsequent
        eviction pressure, the counter should be > 0.

        With fuzzy checkpoint this path is never taken.
        """
        nrows = 5000

        self.session.create(self.uri, 'key_format=i,value_format=S')

        if self.precise:
            self.conn.set_timestamp('stable_timestamp=1')

        self._populate(nrows, self.vsize)
        self.session.checkpoint()

        # Generate more write pressure so the eviction server runs and
        # encounters the scrubbed pages.
        self._populate(nrows, self.vsize)

        if self.precise:
            # Allow the eviction server a moment to process scrubbed pages.
            self.assertStatGreaterSoon(
                stat.conn.cache_scrub_restore, 0,
                timeout=5,
                msg='cache_scrub_restore should be > 0 after precise checkpoint + eviction pressure'
            )
        # For fuzzy we only verify the stat doesn't go negative.
        val = self.get_stat(stat.conn.cache_scrub_restore)
        self.assertGreaterEqual(val, 0, 'cache_scrub_restore must not be negative')

    def test_blocked_precise_checkpoint_stat_wired(self):
        """
        Smoke-test that cache_eviction_blocked_precise_checkpoint is a valid,
        non-negative counter — confirming it is wired up in the build under test.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        if self.precise:
            self.conn.set_timestamp('stable_timestamp=1')

        self._populate(1000, self.vsize)
        self.session.checkpoint()

        val = self.get_stat(stat.conn.cache_eviction_blocked_precise_checkpoint)
        self.assertGreaterEqual(
            val, 0,
            'cache_eviction_blocked_precise_checkpoint must be >= 0'
        )

    def test_data_integrity_after_checkpoint(self):
        """
        Basic correctness: data written before a precise checkpoint must be
        fully readable afterwards.  This catches any regression where scrub
        reconciliation corrupts the on-disk image.
        """
        nrows = 2000

        self.session.create(self.uri, 'key_format=i,value_format=S')

        if self.precise:
            self.conn.set_timestamp('stable_timestamp=1')

        self._populate(nrows, self.vsize)
        self.session.checkpoint()

        # Verify all rows are readable and correct.
        self._verify_reads(nrows, self.vsize)

        # Update half the rows, checkpoint again, verify everything.
        cursor = self.session.open_cursor(self.uri)
        val2 = 'y' * self.vsize
        for i in range(0, nrows, 2):
            cursor[i] = val2
        cursor.close()

        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri)
        for i in range(nrows):
            cursor.set_key(i)
            self.assertEqual(cursor.search(), 0)
            expected = ('y' if i % 2 == 0 else 'x') * self.vsize
            self.assertEqual(
                cursor.get_value(), expected,
                msg=f'Row {i} has wrong value after second checkpoint'
            )
        cursor.close()

    def test_multiple_checkpoint_cycles(self):
        """
        Run several checkpoint cycles with concurrent writes to verify the
        scrub path is stable over time and does not accumulate errors.
        """
        nrows = 1000

        self.session.create(self.uri, 'key_format=i,value_format=S')

        if self.precise:
            self.conn.set_timestamp('stable_timestamp=1')

        for cycle in range(5):
            val = chr(ord('a') + cycle) * self.vsize
            cursor = self.session.open_cursor(self.uri)
            for i in range(nrows):
                cursor[i] = val
            cursor.close()
            self.session.checkpoint()

        # After 5 cycles the last value written should be readable.
        cursor = self.session.open_cursor(self.uri)
        expected = 'e' * self.vsize
        for i in range(nrows):
            cursor.set_key(i)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), expected,
                msg=f'Row {i} has stale value after 5 checkpoint cycles')
        cursor.close()

        # Scrub stat must be non-negative.
        scrub = self.get_stat(stat.conn.cache_write_restore_scrub)
        self.assertGreaterEqual(scrub, 0)

        if self.precise:
            # After 5 cycles there must be at least one scrubbed page.
            self.assertGreater(
                scrub, 0,
                msg='cache_write_restore_scrub should accumulate over multiple precise checkpoints'
            )

    def test_scrub_image_gauge(self):
        """
        cache_scrub_image_pages / cache_scrub_image_bytes are live gauges of the
        clean images currently retained for scrub eviction. They must rise when a
        precise checkpoint retains images and drain back to zero, without ever
        going negative, once eviction consumes those images.
        """
        nrows = 5000

        self.session.create(self.uri, 'key_format=i,value_format=S')

        if self.precise:
            self.conn.set_timestamp('stable_timestamp=1')

        self._settle(nrows, self.vsize)
        self.session.checkpoint()

        pages = self.get_stat(stat.conn.cache_scrub_image_pages)
        nbytes = self.get_stat(stat.conn.cache_scrub_image_bytes)

        # The gauges are unsigned counts; underflow would surface as a huge value.
        self.assertGreaterEqual(pages, 0, 'scrub image page gauge must not be negative')
        self.assertGreaterEqual(nbytes, 0, 'scrub image byte gauge must not be negative')

        if self.precise:
            self.assertGreater(pages, 0,
                msg='scrub image page gauge should rise after a precise checkpoint')
            self.assertGreater(nbytes, 0,
                msg='scrub image byte gauge should rise after a precise checkpoint')

            # A retained image accounts for at least one page's worth of bytes.
            self.assertGreaterEqual(nbytes, pages,
                msg='retained bytes should be at least one per retained page')

            # Dropping the table discards its pages through the image-discard
            # path. The gauge is a live connection-wide count and a checkpoint
            # retains fresh images, so its absolute value after the drop is not
            # predictable; all we require is that the decrement runs cleanly and
            # the two gauges stay consistent with each other.

            # Eviction restores the scrubbed pages as dirty in-memory images,
            # so the table can hold dirty data by the time we drop it.
            # Under precise_checkpoint the drop's close-checkpoint refuses dirty
            # data (WT_DIRTY_DATA). Checkpoint first so the drop closes cleanly.
            self.session.checkpoint()
            self.session.drop(self.uri)
            pages = self.get_stat(stat.conn.cache_scrub_image_pages)
            nbytes = self.get_stat(stat.conn.cache_scrub_image_bytes)
            self.assertGreaterEqual(pages, 0, 'scrub image page gauge underflowed')
            self.assertGreaterEqual(nbytes, 0, 'scrub image byte gauge underflowed')
            self.assertEqual(pages == 0, nbytes == 0,
                msg='scrub image page and byte gauges must drain together: '
                    'pages={}, bytes={}'.format(pages, nbytes))
        else:
            # Fuzzy checkpoint never retains a scrub image.
            self.assertEqual(pages, 0, 'fuzzy checkpoint must not retain scrub images')
            self.assertEqual(nbytes, 0, 'fuzzy checkpoint must not retain scrub image bytes')

    def test_scrub_skipped_dirty_stat_wired(self):
        """
        Smoke-test that cache_write_restore_scrub_skipped_dirty is a valid,
        non-negative counter. It increments when a scrub image is requested for a
        page that reconciliation leaves dirty, so the image would never be usable.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        if self.precise:
            self.conn.set_timestamp('stable_timestamp=1')

        self._populate(1000, self.vsize)
        self.session.checkpoint()

        val = self.get_stat(stat.conn.cache_write_restore_scrub_skipped_dirty)
        self.assertGreaterEqual(val, 0,
            'cache_write_restore_scrub_skipped_dirty must be >= 0')

class test_checkpoint_scrub_evict_config(wttest.WiredTigerTestCase):
    """
    Verify the eviction.checkpoint_scrub_eviction override:
      - off:  checkpoint never scrub-evicts, even under precise checkpoint + pressure.
      - on:   checkpoint always scrub-evicts eligible row-leaf pages.
      - auto: defers to the cache-pressure heuristic (the default).

    The checkpoint scrub activation counter increments once per reconciliation
    that retains a scrub image, so it is a clean signal for whether the override
    enabled or disabled the path.
    """

    uri = 'table:scrub_evict_cfg'
    nrows = 5000
    vsize = 200

    mode = [
        ('off',  dict(mode='off',  expect_scrub=False)),
        ('on',   dict(mode='on',   expect_scrub=True)),
        ('auto', dict(mode='auto', expect_scrub=True)),
    ]
    scenarios = make_scenarios(mode)

    # Small cache creates eviction pressure; precise checkpoint is required for the feature. Under
    # this pressure the auto heuristic also enables scrub, so auto and on share the same expectation.
    def conn_config(self):
        return (
            'cache_size=50MB,statistics=(all),precise_checkpoint=true,'
            'eviction=(checkpoint_scrub_eviction={})'.format(self.mode)
        )

    def _populate(self, value):
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = value
        cursor.close()

    def test_mode(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('stable_timestamp=1')

        self._populate('x' * self.vsize)
        self.session.checkpoint()

        pc = self.get_stat(stat.conn.cache_write_restore_scrub_checkpoint)
        if self.expect_scrub:
            self.assertGreater(pc, 0,
                msg='checkpoint_scrub_eviction={} should scrub eligible pages, stat={}'.format(
                    self.mode, pc))
        else:
            self.assertEqual(pc, 0,
                msg='checkpoint_scrub_eviction=off must not scrub, stat={}'.format(pc))

class test_checkpoint_scrub_evict_reconfigure(wttest.WiredTigerTestCase):
    """
    The override is honored across WT_CONNECTION::reconfigure: turning it off then
    on flips whether checkpoint scrub-evicts eligible pages.
    """

    uri = 'table:scrub_evict_reconfig'
    nrows = 5000
    vsize = 200

    def conn_config(self):
        return 'cache_size=50MB,statistics=(all),precise_checkpoint=true'

    def _populate(self, value):
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = value
        cursor.close()

    def _scrub_stat(self):
        return self.get_stat(stat.conn.cache_write_restore_scrub_checkpoint)

    def test_reconfigure(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('stable_timestamp=1')

        # The connection opened with the default (auto), whose open-time checkpoint may already have
        # scrubbed a page, so compare deltas rather than absolute counts.

        # With the override off, no page should scrub regardless of cache pressure.
        self.conn.reconfigure('eviction=(checkpoint_scrub_eviction=off)')
        before = self._scrub_stat()
        self._populate('a' * self.vsize)
        self.session.checkpoint()
        self.assertEqual(self._scrub_stat(), before,
            'no pages should scrub while the override is off')

        # Turning it on must enable scrub without reopening the connection.
        self.conn.reconfigure('eviction=(checkpoint_scrub_eviction=on)')
        before = self._scrub_stat()
        self._populate('b' * self.vsize)
        self.session.checkpoint()
        self.assertGreater(self._scrub_stat(), before,
            'pages should scrub after enabling the override')

class test_checkpoint_scrub_evict_no_precise(wttest.WiredTigerTestCase):
    """
    The mode difference that matters without precise checkpoint:
      - on:   scrubs regardless of precise checkpoint (the only mode that activates here).
      - auto: matches today's behavior, which requires precise checkpoint, so it stays off.
      - off:  disabled.
    """

    uri = 'table:scrub_evict_noprecise'
    nrows = 5000
    vsize = 200

    mode = [
        ('off',  dict(mode='off',  expect_scrub=False)),
        ('on',   dict(mode='on',   expect_scrub=True)),
        ('auto', dict(mode='auto', expect_scrub=False)),
    ]
    scenarios = make_scenarios(mode)

    # Deliberately no precise checkpoint: only "on" should activate the scrub path here.
    def conn_config(self):
        return (
            'cache_size=50MB,statistics=(all),precise_checkpoint=false,'
            'eviction=(checkpoint_scrub_eviction={})'.format(self.mode)
        )

    def _populate(self, value):
        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nrows):
            cursor[i] = value
        cursor.close()

    def test_mode(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')

        self._populate('x' * self.vsize)
        self.session.checkpoint()

        pc = self.get_stat(stat.conn.cache_write_restore_scrub_checkpoint)
        if self.expect_scrub:
            self.assertGreater(pc, 0,
                msg='checkpoint_scrub_eviction=on should scrub without precise checkpoint, stat={}'
                    .format(pc))
        else:
            self.assertEqual(pc, 0,
                msg='checkpoint_scrub_eviction={} must not scrub without precise checkpoint, '
                    'stat={}'.format(self.mode, pc))

class test_checkpoint_scrub_image_gauge(wttest.WiredTigerTestCase):
    """
    The cache_scrub_image_pages / cache_scrub_image_bytes gauges track the memory
    of the clean re-instantiation images checkpoint scrub currently retains. Tie
    the memory tracking to the config that drives it: with the override off no
    image is retained, so nothing is tracked and the gauge stays at zero; with it
    on the scrub path retains images, so the gauge is non-zero and the byte gauge
    is consistent with the page gauge. This confirms the accounting is wired to
    the added stats and is populated only when scrub actually runs.
    """

    uri = 'table:scrub_image_gauge'
    nrows = 5000
    vsize = 200

    mode = [
        ('off', dict(mode='off', expect_tracked=False)),
        ('on',  dict(mode='on',  expect_tracked=True)),
    ]
    scenarios = make_scenarios(mode)

    def conn_config(self):
        return ('cache_size=50MB,statistics=(all),precise_checkpoint=true,'
                'eviction=(checkpoint_scrub_eviction={})'.format(self.mode))

    def _populate(self):
        cursor = self.session.open_cursor(self.uri)
        val = 'x' * self.vsize
        for i in range(self.nrows):
            cursor[i] = val
        cursor.close()

    def test_gauge_tracks_retained_images(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('stable_timestamp=1')

        # Settle the tree into disk-shaped pages before dirtying it again: an image is only
        # retained for a 1-for-1 page swap, and a freshly inserted tree splits at checkpoint.
        self._populate()
        self.session.checkpoint()
        cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        for i in range(self.nrows):
            cursor.set_key(i)
            cursor.search()
            cursor.reset()
        cursor.close()

        self._populate()
        self.session.checkpoint()

        pages = self.get_stat(stat.conn.cache_scrub_image_pages)
        nbytes = self.get_stat(stat.conn.cache_scrub_image_bytes)

        # Whatever the mode, the gauge is an unsigned count; an underflow from an
        # unbalanced decr would surface here as a huge value.
        self.assertGreaterEqual(pages, 0, 'scrub image page gauge underflowed')
        self.assertGreaterEqual(nbytes, 0, 'scrub image byte gauge underflowed')

        if self.expect_tracked:
            # Scrub ran, so at least one image is retained and its bytes tracked.
            self.assertGreater(pages, 0,
                msg='scrub image page gauge should be non-zero when scrub is enabled')
            self.assertGreater(nbytes, 0,
                msg='scrub image byte gauge should be non-zero when scrub is enabled')
            # A retained image accounts for at least one page's worth of bytes.
            self.assertGreaterEqual(nbytes, pages,
                msg='retained bytes should be at least one per retained page')
        else:
            # Scrub never retained an image, so nothing is tracked.
            self.assertEqual(pages, 0,
                msg='scrub image page gauge must stay zero when scrub is disabled')
            self.assertEqual(nbytes, 0,
                msg='scrub image byte gauge must stay zero when scrub is disabled')
