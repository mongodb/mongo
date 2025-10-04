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

import os, random, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtdataset import TrackedSimpleDataSet, TrackedComplexDataSet
from wtscenario import make_scenarios

# test_tiered14.py
#    Test somewhat arbitrary combinations of flush_tier, checkpoint, restarts,
#    data additions and updates.
class test_tiered14(wttest.WiredTigerTestCase, TieredConfigMixin):

    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered14', tiered_only=True)

    uri = "table:test_tiered14-{}"   # format for subtests

    # The multiplier makes the size of keys and values progressively larger.
    # A multiplier of 0 makes the keys and values a single length.
    multiplier = [
        ('0', dict(multiplier=0)),
        ('S', dict(multiplier=1)),
        ('M', dict(multiplier=10)),
        ('L', dict(multiplier=100, long_only=True)),
        ('XL', dict(multiplier=1000, long_only=True)),
    ]
    keyfmt = [
        ('integer', dict(keyfmt='i')),
        ('string', dict(keyfmt='S')),
    ]
    dataset = [
        ('simple', dict(dataset='simple')),
        #('complex', dict(dataset='complex', long_only=True)),
    ]
    scenarios = make_scenarios(multiplier, keyfmt, dataset, storage_sources)

    def conn_config(self):
        return TieredConfigMixin.conn_config(self)

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    def progress(self, s):
        outstr = "testnum {}, position {}: {}".format(self.testnum, self.position, s)
        self.verbose(3, outstr)
        self.pr(outstr)

    # Run a sequence of operations, indicated by a string.
    #  a = add some number of keys
    #  u = update some number of keys
    #  c = checkpoint
    #  r = reopen
    #  f = flush_tier
    #  . = check to make sure all expected values are present
    #
    # We require a unique test number so we get can generate a different uri from
    # previous runs.  A different approach is to drop the uri, but then we need to
    # remove the bucket and cache, which is specific to the storage source extension.
    def playback(self, testnum, ops):
        self.testnum = testnum
        self.position = -1

        uri = self.uri.format(testnum)
        self.progress('Running ops: {} using uri {}'.format(ops, uri))
        if self.dataset == 'simple':
            ds = TrackedSimpleDataSet(self, uri, self.multiplier,
                                      key_format = self.keyfmt)
        elif self.dataset == 'complex':
            ds = TrackedComplexDataSet(self, uri, self.multiplier,
                                      key_format = self.keyfmt)

        # Populate for a tracked data set is needed to create the uri.
        ds.populate()
        inserted = 0

        # At the end of the sequence of operations, do a final check ('.').
        for op in ops + '.':
            self.position += 1
            try:
                if op == 'f':
                    self.progress('flush_tier')
                    self.session.checkpoint('flush_tier=(enabled)')
                elif op == 'c':
                    self.progress('checkpoint')
                    self.session.checkpoint()
                elif op == 'r':
                    self.progress('reopen')
                    self.reopen_conn()
                elif op == 'a':
                    self.progress('add')
                    n = random.randrange(1, 101)  # 1 <= n <= 100
                    ds.store_range(inserted, n)
                    inserted += n
                elif op == 'u':
                    self.progress('update')
                    # only update the elements if enough have already been added.
                    n = random.randrange(1, 101)  # 1 <= n <= 100
                    if n < inserted:
                        pos = random.randrange(0, inserted - n)
                        ds.store_range(pos, n)
                elif op == '.':
                    self.progress('check')
                    ds.check()
            except Exception as e:
                self.progress('Failed at position {} in {}: {}'.format(self.position, ops, str(e)))
                raise(e)

    # Test tiered storage with checkpoints and flush_tier calls.
    def test_tiered(self):
        random.seed(0)

        # Get started with a fixed sequence of basic operations.
        # There's no particular reason to start with this sequence.
        testnum = 0
        self.playback(testnum, "aaaaacaaa.uucrauaf.aauaac.auu.aacrauafa.uruua.")

        for i in range(0, 10):
            testnum += 1
            # Generate a sequence of operations that is heavy on additions and updates.
            s = ''.join(random.choices('aaaaauuuuufcr.', k=self.num_ops))
            self.playback(testnum, s)

        for i in range(0, 10):
            testnum += 1
            # Generate a sequence of operations that is has a greater mix of 'operational' functions.
            s = ''.join(random.choices('aufcr.', k=self.num_ops))
            self.playback(testnum, s)
