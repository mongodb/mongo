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

from helper_tiered import generate_s3_prefix, get_auth_token, get_bucket1_name
from wtscenario import make_scenarios
import os, wiredtiger, wttest
StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered11.py
#    Test flush time and flush timestamp in metadata.
class test_tiered11(wttest.WiredTigerTestCase):
    storage_sources = [
        ('local', dict(auth_token = get_auth_token('local_store'),
            bucket = get_bucket1_name('local_store'),
            bucket_prefix = "pfx_",
            ss_name = 'local_store')),
        ('s3', dict(auth_token = get_auth_token('s3_store'),
            bucket = get_bucket1_name('s3_store'),
            bucket_prefix = generate_s3_prefix(),
            ss_name = 's3_store'))
    ]
    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    # If the 'uri' changes all the other names must change with it.
    base = 'test_tiered11-000000000'
    nentries = 10
    objuri = 'object:' + base + '1.wtobj'
    tiereduri = "tiered:test_tiered11"
    uri = "table:test_tiered11"

    def conn_config(self):
        if self.ss_name == 'local_store' and not os.path.exists(self.bucket):
            os.mkdir(self.bucket)
        self.saved_conn = \
          'statistics=(all),' + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.bucket_prefix + \
          'name=%s)' % self.ss_name 
        return self.saved_conn

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        config = ''
        # S3 store is built as an optional loadable extension, not all test environments build S3.
        if self.ss_name == 's3_store':
            #config = '=(config=\"(verbose=1)\")'
            extlist.skip_if_missing = True
        #if self.ss_name == 'local_store':
            #config = '=(config=\"(verbose=1,delay_ms=200,force_delay=3)\")'
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('storage_sources', self.ss_name + config)

    # Check for a specific string as part of the uri's metadata.
    def check_metadata(self, uri, val_str, match=True):
        #self.pr("Check_meta: uri: " + uri)
        c = self.session.open_cursor('metadata:')
        val = c[uri]
        c.close()
        #self.pr("Check_meta: metadata val: " + val)
        if match:
            #self.pr("Check_meta: Should see val_str: " + val_str)
            self.assertTrue(val_str in val)
        else:
            #self.pr("Check_meta: Should not see val_str: " + val_str)
            self.assertFalse(val_str in val)

    def add_data(self, start):
        c = self.session.open_cursor(self.uri)
        # Begin by adding some data.
        end = start + self.nentries
        for i in range(start, end):
            self.session.begin_transaction()
            c[i] = i
            # Jump the commit TS to leave rooom for the stable TS separate from any commit TS.
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i * 2))
        # Set the oldest and stable timestamp to the end.
        end_ts = self.timestamp_str(end-1)
        self.conn.set_timestamp('oldest_timestamp=' + end_ts + ',stable_timestamp=' + end_ts)
        c.close()
        return end_ts

    # Test calling the flush_tier API.
    def test_tiered11(self):
        # Create a tiered table and checkpoint. Make sure the recorded
        # timestamp is what we expect.
        intl_page = 'internal_page_max=16K'
        base_create = 'key_format=i,value_format=i,' + intl_page
        self.session.create(self.uri, base_create)

        end_ts = self.add_data(1)
        self.session.checkpoint()

        new_end_ts = self.add_data(self.nentries)
        # We have a new stable timestamp, but after the checkpoint. Make
        # sure the flush tier records the correct timestamp.
        self.session.flush_tier(None)
        # Make sure a new checkpoint doesn't change any of our timestamp info.
        self.session.checkpoint()

        flush_str = 'flush_timestamp="' + end_ts + '"'
        self.check_metadata(self.tiereduri, flush_str)
        self.check_metadata(self.objuri, flush_str)
        # Make sure some flush time was saved. We don't know what it is other
        # than it should not be zero.
        time_str = "flush_time=0"
        self.check_metadata(self.tiereduri, time_str, False)
        self.check_metadata(self.objuri, time_str, False)

if __name__ == '__main__':
    wttest.run()
