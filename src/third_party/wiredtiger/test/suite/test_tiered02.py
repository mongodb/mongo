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

import os, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtdataset import SimpleDataSet, ComplexDataSet
from wtscenario import make_scenarios

# test_tiered02.py
#    Test tiered tree
class test_tiered02(wttest.WiredTigerTestCase, TieredConfigMixin):
    complex_dataset = [
        ('simple_ds', dict(complex_dataset=False)),
        ('complex_ds', dict(complex_dataset=True)),
    ]

    # Make scenarios for different cloud service providers
    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered02', tiered_only=True)
    scenarios = make_scenarios(storage_sources, complex_dataset)

    uri = "table:test_tiered02"

    def conn_config(self):
        return TieredConfigMixin.conn_config(self)

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    def progress(self, s):
        self.verbose(3, s)
        self.pr(s)

    def confirm_flush(self, increase=True):
        # Without directly using the filesystem API, directory listing is only supported on
        # the directory store.  Limit this check to the directory store.
        if self.ss_name != 'dir_store':
            return

        got = sorted(list(os.listdir(self.bucket)))
        self.pr('Flushed objects: ' + str(got))
        if increase:
            # WT-7639: we know that this assertion sometimes fails,
            # we are collecting more data - we still want it to fail
            # so it is noticed.
            if len(got) <= self.flushed_objects:
                from time import sleep
                self.prout('directory items: {} is not greater than {}!'.
                  format(got, self.flushed_objects))
                self.prout('waiting to see if it resolves')
                for i in range(0, 10):
                    self.prout('checking again')
                    newgot = sorted(list(os.listdir(self.bucket)))
                    if len(newgot) > self.flushed_objects:
                        self.prout('resolved, now see: {}'.format(newgot))
                        break
                    sleep(i)
            self.assertGreater(len(got), self.flushed_objects)
        else:
            self.assertEqual(len(got), self.flushed_objects)
        self.flushed_objects = len(got)

    def get_dataset(self, rows):
        args = 'key_format=S'

        if self.complex_dataset:
            return ComplexDataSet(self, self.uri, rows, config=args)
        else:
            return SimpleDataSet(self, self.uri, rows, config=args)

    # Test tiered storage with checkpoints and flush_tier calls.
    def test_tiered(self):
        self.flushed_objects = 0

        self.pr("create sys")
        self.progress('Create simple data set (10)')
        ds = self.get_dataset(10)
        self.progress('populate')
        ds.populate()
        ds.check()
        self.progress('checkpoint')
        self.session.checkpoint()
        self.progress('flush_tier')
        self.session.checkpoint('flush_tier=(enabled)')
        self.confirm_flush()
        ds.check()

        self.close_conn()
        self.progress('reopen_conn')
        self.reopen_conn()
        # Check what was there before.
        ds = self.get_dataset(10)
        ds.check()

        self.progress('Create simple data set (50)')
        ds = self.get_dataset(50)
        self.progress('populate')
        # Don't (re)create any of the tables or indices from here on out.
        # We will keep a cursor open on the table, and creation requires
        # exclusive access.
        ds.populate(create=False)
        ds.check()
        self.progress('open extra cursor on ' + self.uri)
        cursor = self.session.open_cursor(self.uri, None, None)
        self.progress('checkpoint')
        self.session.checkpoint()

        self.progress('flush_tier')
        self.session.checkpoint('flush_tier=(enabled)')
        self.progress('flush_tier complete')
        self.confirm_flush()

        self.progress('Create simple data set (100)')
        ds = self.get_dataset(100)
        self.progress('populate')
        ds.populate(create=False)
        ds.check()
        self.progress('checkpoint')
        self.session.checkpoint()
        self.progress('flush_tier')
        self.session.checkpoint('flush_tier=(enabled)')
        self.confirm_flush()

        self.progress('Create simple data set (200)')
        ds = self.get_dataset(200)
        self.progress('populate')
        ds.populate(create=False)
        ds.check()
        cursor.close()
        self.progress('close_conn')
        self.close_conn()

        self.progress('reopen_conn')
        self.reopen_conn()

        # Check what was there before.
        ds = self.get_dataset(200)
        ds.check()

        # Now add some more.
        self.progress('Create simple data set (300)')
        ds = self.get_dataset(300)
        self.progress('populate')
        ds.populate(create=False)
        ds.check()

        # We haven't done a flush so there should be
        # nothing extra on the shared tier.
        self.confirm_flush(increase=False)
        self.progress('checkpoint')
        self.session.checkpoint()
        self.confirm_flush(increase=False)
        self.progress('END TEST')

if __name__ == '__main__':
    wttest.run()
