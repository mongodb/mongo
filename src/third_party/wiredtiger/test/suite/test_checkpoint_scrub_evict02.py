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
# test_checkpoint_scrub_evict02.py
#
# Cache accounting for the disk images checkpoint retains for scrub eviction.
#
# An image counts towards cache->bytes_scrub_image, cache->pages_scrub_image and the page's memory
# footprint while checkpoint reconciliation keeps it, and is released when eviction consumes it, a
# later reconciliation replaces it, or the page is freed. These tests drive each of those release
# paths and check the counters return to zero. A diagnostic build aborts on an accounting
# underflow, and the cache reports non-zero counters when it is destroyed.

import threading

import wttest
from wiredtiger import stat, WiredTigerError, wiredtiger_strerror, WT_ROLLBACK
from wtscenario import make_scenarios
from wtthread import Thread

class test_checkpoint_scrub_evict02(wttest.WiredTigerTestCase):
    uri = 'table:scrub_evict02'
    nrows = 5000

    ckpt_threads = [
        ('serial', dict(ckpt_threads=1)),
        ('parallel', dict(ckpt_threads=4)),
    ]
    scenarios = make_scenarios(ckpt_threads)

    # A large cache so eviction only runs when a test asks for it, and scrub eviction forced on so
    # the tests don't depend on the eviction server's scrub heuristic. The dirty thresholds are
    # raised as the default 5% target causes the eviction server to run alongside the some tests,
    # which can race with the retained images.
    def conn_config(self):
        return ('cache_size=200MB,statistics=(all),precise_checkpoint=true,'
                'checkpoint_threads=%d,'
                'eviction_dirty_target=80,eviction_dirty_trigger=90,'
                'eviction=(checkpoint_scrub_eviction=on)' % self.ckpt_threads)

    def get_stat(self, statistic, uri=None):
        cursor = self.session.open_cursor('statistics:' if uri is None else 'statistics:' + uri)
        value = cursor[statistic][2]
        cursor.close()
        return value

    def scrub_images(self):
        return (self.get_stat(stat.conn.cache_scrub_image_pages),
          self.get_stat(stat.conn.cache_scrub_image_bytes))

    def create(self, config=''):
        self.session.create(self.uri, 'key_format=i,value_format=S' + config)
        self.conn.set_timestamp('stable_timestamp=1')

    def write(self, value, nrows=None):
        cursor = self.session.open_cursor(self.uri)
        for i in range(nrows if nrows is not None else self.nrows):
            cursor[i] = value
        cursor.close()

    def check_values(self, value, nrows=None):
        cursor = self.session.open_cursor(self.uri)
        for i in range(nrows if nrows is not None else self.nrows):
            cursor.set_key(i)
            self.assertEqual(cursor.search(), 0)
            self.assertEqual(cursor.get_value(), value)
        cursor.close()

    def evict_all(self, nrows=None):
        cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        for i in range(nrows if nrows is not None else self.nrows):
            cursor.set_key(i)
            cursor.search()
            cursor.reset()
        cursor.close()

    def settle(self, nrows=None, value='x' * 100, config=''):
        """Lay the tree out as disk-shaped leaf pages so we can update
           a tree that already has its final shape."""
        self.create(config)
        self.write(value, nrows)
        self.session.checkpoint()
        self.evict_all(nrows)

    def check_consistent(self):
        """The page and byte counters must rise and fall together."""
        pages, image_bytes = self.scrub_images()
        self.assertEqual(pages == 0, image_bytes == 0,
          'scrub image pages %d inconsistent with bytes %d' % (pages, image_bytes))
        if pages != 0:
            # An image is a leaf page's disk image: neither empty nor implausibly large.
            self.assertGreater(image_bytes // pages, 32)
            self.assertLess(image_bytes // pages, 10 * 1024 * 1024)

    def test_images_retained_and_released_by_eviction(self):
        """Eviction consumes the retained images, releasing their accounting."""
        self.settle()
        self.write('y' * 100)
        self.session.checkpoint()

        pages, image_bytes = self.scrub_images()
        self.assertGreater(pages, 0, 'checkpoint retained no scrub images')
        self.assertGreater(image_bytes, 0)
        self.check_consistent()

        # The pages are clean with a retained image, so eviction re-instantiates them in cache
        # rather than discarding them, and the accounting for the consumed image is released.
        self.evict_all()
        self.assertGreater(self.get_stat(stat.conn.cache_scrub_restore), 0)
        self.assertEqual(self.scrub_images(), (0, 0))
        self.check_values('y' * 100)

    def test_images_released_when_page_discarded(self):
        """Dropping the table frees pages that still hold a retained image."""
        self.settle()
        self.write('y' * 100)
        self.session.checkpoint()
        self.assertGreater(self.scrub_images()[0], 0)

        self.session.drop(self.uri)
        self.assertEqual(self.scrub_images(), (0, 0))

    def test_images_released_by_later_reconciliation(self):
        """A page dirtied and reconciled again releases the image its last checkpoint retained.

        Nothing counts the image as dirty when it is retained, because the page is clean by then.
        Dirtying the page later counts its whole footprint, image included, and the reconciliation
        that replaces the image is what takes those bytes back out. Three different points, so
        they are easy to get out of step."""
        self.settle()

        for cycle in range(5):
            self.write('y' * (100 + cycle))
            self.session.checkpoint()
            self.check_consistent()
            self.assertGreater(self.scrub_images()[0], 0,
              'no images retained on cycle %d' % cycle)

        self.evict_all()
        self.assertEqual(self.scrub_images(), (0, 0))
        self.check_values('y' * 104)

    def test_images_released_on_close(self):
        """Closing with images still retained must not leave the counters non-zero.

        The cache reports leftover bytes or pages to stderr when it is destroyed, which fails the
        test through the framework's stderr check."""
        self.settle()
        self.write('y' * 100)
        self.session.checkpoint()
        self.assertGreater(self.scrub_images()[0], 0)

        self.reopen_conn()
        self.check_values('y' * 100)

    def test_no_images_retained_when_disabled(self):
        """checkpoint_scrub_eviction=off retains nothing."""
        self.conn.reconfigure('eviction=(checkpoint_scrub_eviction=off)')
        self.settle()
        self.write('y' * 100)
        self.session.checkpoint()
        self.assertEqual(self.scrub_images(), (0, 0))

        self.conn.reconfigure('eviction=(checkpoint_scrub_eviction=on)')
        self.write('z' * 100)
        self.session.checkpoint()
        self.assertGreater(self.scrub_images()[0], 0)

    def test_image_max_zero_retains_nothing(self):
        """A zero image budget disables retention even with scrub eviction on."""
        self.conn.reconfigure('eviction=(checkpoint_scrub_image_max=0)')
        self.settle()
        self.write('y' * 100)
        self.session.checkpoint()
        self.assertEqual(self.scrub_images(), (0, 0))

    def test_image_max_bounds_the_cache(self):
        """The retained images stay within the configured percentage of the cache.

        Reconciliation applies the budget as it builds each image, so the overshoot is bounded by
        the images being built concurrently: one per checkpoint reconciliation thread. Applying it
        where checkpoint asks for the image instead would let a parallel checkpoint queue a whole
        tree's worth of pages before any of them consumed the budget."""
        leaf_page_max = 32 * 1024
        nrows = 20000

        self.conn.reconfigure('eviction=(checkpoint_scrub_image_max=1)')
        # Roughly 20MB of data against a 1% budget.
        self.settle(nrows=nrows, value='v' * 1000, config=',leaf_page_max=32KB')
        self.write('w' * 1000, nrows)
        self.session.checkpoint()

        budget = self.get_stat(stat.conn.cache_bytes_max) // 100
        pages, image_bytes = self.scrub_images()
        self.assertGreater(pages, 0, 'checkpoint retained no scrub images')
        self.assertLessEqual(image_bytes, budget + self.ckpt_threads * leaf_page_max,
          'retained %d bytes of scrub images, over the %d byte cache budget'
          % (image_bytes, budget))

    def test_updates_concurrent_with_checkpoint(self):
        """Accounting must survive pages being re-dirtied under a running checkpoint.

        Reconciliation accounts for the image after settling the page's state, so a page dirtied
        mid-reconciliation takes the other branch: the image lands in the dirty totals and stays
        there until it is released. The cache verifies its counters when the connection closes,
        which the test framework surfaces as unexpected stderr."""
        self.settle()

        stop = threading.Event()
        failures = []

        def writer(base):
            try:
                session = self.conn.open_session()
                cursor = session.open_cursor(self.uri)
                value = 'w' * 100
                while not stop.is_set():
                    for i in range(base, self.nrows, 4):
                        try:
                            cursor[i] = value
                        except WiredTigerError as e:
                            if wiredtiger_strerror(WT_ROLLBACK) not in str(e):
                                raise
                cursor.close()
                session.close()
            except Exception as e:
                failures.append(e)

        threads = [Thread(target=writer, args=(base,)) for base in range(4)]
        for thread in threads:
            thread.start()
        try:
            for _ in range(10):
                self.session.checkpoint()
                self.check_consistent()
        finally:
            stop.set()
            for thread in threads:
                thread.join()
        self.assertEqual(failures, [])

        self.session.checkpoint()
        self.evict_all()
        self.check_consistent()
        self.check_values('w' * 100)

    def test_split_at_checkpoint_retains_no_image(self):
        """A page that splits during checkpoint reconciliation must not retain its chunk images.

        There is no consumer for them: the swap path only handles a 1-for-1 page swap. Worse, the
        eviction that follows hands every chunk to __wt_multi_to_ref, which instantiates a page in
        cache for each chunk that carries an image instead of leaving the parent pointing at the
        on-disk block, so evicting the page would free nothing."""
        # Large pages in memory, small pages on disk: reconciliation has to split.
        self.create(',leaf_page_max=4KB,memory_page_max=10MB')
        self.write('x' * 200, nrows=2000)
        self.session.checkpoint()

        self.assertGreater(self.get_stat(stat.dsrc.rec_multiblock_leaf, self.uri), 0,
          'workload did not split a leaf page during checkpoint')
        self.assertEqual(self.scrub_images(), (0, 0))

        # Evict across the key range, then measure what the split left behind. Which thread
        # realizes the split, and when, is not ours to choose: eviction may take the page while
        # the checkpoint is still running, or only once the cursor forces it. So drive eviction
        # over the whole range and check the end state rather than any single eviction.
        self.evict_all(2000)

        self.assertGreater(self.get_stat(stat.dsrc.cache_eviction_split_leaf, self.uri), 0,
          'eviction never realized the checkpoint split')

        # A chunk that kept its image is instantiated in cache instead of being left on disk, which
        # counts a restore. Nothing here retains an image, so any restore is a chunk's.
        self.assertEqual(self.get_stat(stat.dsrc.cache_scrub_restore, self.uri), 0,
          'a chunk of the split page was instantiated from an image')

        # The same property from the cache's side: a page per chunk would leave the whole tree in
        # cache. The tree's leaf pages count the chunks the split produced.
        chunks = self.get_stat(stat.dsrc.btree_row_leaf, self.uri)
        pages = self.get_stat(stat.conn.cache_pages_inuse)
        self.assertGreater(chunks, 20, 'the checkpoint split the page into too few chunks to tell')
        self.assertLess(pages, chunks // 4,
          'a split of %d chunks left %d pages in cache' % (chunks, pages))
        self.check_values('x' * 200, nrows=2000)
