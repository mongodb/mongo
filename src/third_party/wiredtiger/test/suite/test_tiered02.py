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
from helper_tiered import generate_s3_prefix, get_auth_token, get_bucket1_name
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_tiered02.py
#    Test tiered tree
class test_tiered02(wttest.WiredTigerTestCase):
    storage_sources = [
        ('dirstore', dict(auth_token = get_auth_token('dir_store'),
            bucket = get_bucket1_name('dir_store'),
            bucket_prefix = "pfx_",
            ss_name = 'dir_store')),
        ('s3', dict(auth_token = get_auth_token('s3_store'),
            bucket = get_bucket1_name('s3_store'),
            bucket_prefix = generate_s3_prefix(),
            ss_name = 's3_store')),
    ]
    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    uri = "table:test_tiered02"

    def conn_config(self):
        if self.ss_name == 'dir_store' and not os.path.exists(self.bucket):
            os.mkdir(self.bucket)
        return \
          'debug_mode=(flush_checkpoint=true),' + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.bucket_prefix + \
          'name=%s),tiered_manager=(wait=0)' % self.ss_name

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        config = ''
        # S3 store is built as an optional loadable extension, not all test environments build S3.
        if self.ss_name == 's3_store':
            #config = '=(config=\"(verbose=1)\")'
            extlist.skip_if_missing = True
        #if self.ss_name == 'dir_store':
            #config = '=(config=\"(verbose=1,delay_ms=200,force_delay=3)\")'
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.ss_name + config)

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

    # Test tiered storage with checkpoints and flush_tier calls.
    def test_tiered(self):
        self.flushed_objects = 0
        args = 'key_format=S'

        intl_page = 'internal_page_max=16K'
        base_create = 'key_format=S,value_format=S,' + intl_page
        self.pr("create sys")
        #self.session.create(self.uri + 'xxx', base_create)

        self.progress('Create simple data set (10)')
        ds = SimpleDataSet(self, self.uri, 10, config=args)
        self.progress('populate')
        ds.populate()
        ds.check()
        self.progress('checkpoint')
        self.session.checkpoint()
        self.progress('flush_tier')
        self.session.flush_tier(None)
        self.confirm_flush()
        ds.check()

        self.close_conn()
        self.progress('reopen_conn')
        self.reopen_conn()
        # Check what was there before.
        ds = SimpleDataSet(self, self.uri, 10, config=args)
        ds.check()

        self.progress('Create simple data set (50)')
        ds = SimpleDataSet(self, self.uri, 50, config=args)
        self.progress('populate')
        ds.populate()
        ds.check()
        self.progress('open extra cursor on ' + self.uri)
        cursor = self.session.open_cursor(self.uri, None, None)
        self.progress('checkpoint')
        self.session.checkpoint()

        self.progress('flush_tier')
        self.session.flush_tier(None)
        self.progress('flush_tier complete')
        self.confirm_flush()

        self.progress('Create simple data set (100)')
        ds = SimpleDataSet(self, self.uri, 100, config=args)
        self.progress('populate')
        ds.populate()
        ds.check()
        self.progress('checkpoint')
        self.session.checkpoint()
        self.progress('flush_tier')
        self.session.flush_tier(None)
        self.confirm_flush()

        self.progress('Create simple data set (200)')
        ds = SimpleDataSet(self, self.uri, 200, config=args)
        self.progress('populate')
        ds.populate()
        ds.check()
        cursor.close()
        self.progress('close_conn')
        self.close_conn()

        self.progress('reopen_conn')
        self.reopen_conn()

        # Check what was there before.
        ds = SimpleDataSet(self, self.uri, 200, config=args)
        ds.check()

        # Now add some more.
        self.progress('Create simple data set (300)')
        ds = SimpleDataSet(self, self.uri, 300, config=args)
        self.progress('populate')
        ds.populate()
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
