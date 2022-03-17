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

import os, time, wiredtiger, wttest
from wiredtiger import stat
from helper_tiered import generate_s3_prefix, get_auth_token
from helper_tiered import get_bucket1_name, get_bucket2_name
from wtscenario import make_scenarios
StorageSource = wiredtiger.StorageSource  # easy access to constants

# test_tiered04.py
#    Basic tiered storage API test.
class test_tiered04(wttest.WiredTigerTestCase):
    storage_sources = [
        ('dir_store', dict(auth_token = get_auth_token('dir_store'),
            bucket = get_bucket1_name('dir_store'),
            bucket1 = get_bucket2_name('dir_store'),
            prefix = "pfx_",
            prefix1 = "pfx1_",
            ss_name = 'dir_store')),
        ('s3', dict(auth_token = get_auth_token('s3_store'),
            bucket = get_bucket1_name('s3_store'),
            bucket1 = get_bucket2_name('s3_store'),
            prefix = generate_s3_prefix(),
            prefix1 = generate_s3_prefix(),
            ss_name = 's3_store')),
    ]
    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(storage_sources)

    # If the 'uri' changes all the other names must change with it.
    base = 'test_tiered04-000000000'
    fileuri_base = 'file:' + base
    obj1file = base + '1.wtobj'
    obj2file = base + '2.wtobj'
    objuri = 'object:' + base + '1.wtobj'
    tiereduri = "tiered:test_tiered04"
    uri = "table:test_tiered04"

    uri1 = "table:test_other_tiered04"
    uri_none = "table:test_local04"

    object_sys = "9M"
    object_sys_val = 9 * 1024 * 1024
    object_uri = "15M"
    object_uri_val = 15 * 1024 * 1024
    retention = 3
    retention1 = 600
    def conn_config(self):
        if self.ss_name == 'dir_store':
            os.mkdir(self.bucket)
            os.mkdir(self.bucket1)
        self.saved_conn = \
          'debug_mode=(flush_checkpoint=true),' + \
          'statistics=(all),' + \
          'tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket + \
          'bucket_prefix=%s,' % self.prefix + \
          'local_retention=%d,' % self.retention + \
          'name=%s,' % self.ss_name + \
          'object_target_size=%s)' % self.object_sys
        return self.saved_conn

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

    # Check for a specific string as part of the uri's metadata.
    def check_metadata(self, uri, val_str):
        c = self.session.open_cursor('metadata:')
        val = c[uri]
        c.close()
        self.assertTrue(val_str in val)

    def get_stat(self, stat, uri):
        if uri == None:
            stat_cursor = self.session.open_cursor('statistics:')
        else:
            stat_cursor = self.session.open_cursor('statistics:' + uri)
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def check(self, tc, n):
        for i in range(0, n):
            self.assertEqual(tc[str(i)], str(i))
        tc.set_key(str(n))
        self.assertEquals(tc.search(), wiredtiger.WT_NOTFOUND)

    # Test calling the flush_tier API.
    def test_tiered(self):
        # Create three tables. One using the system tiered storage, one
        # specifying its own bucket and object size and one using no
        # tiered storage. Use stats to verify correct setup.
        intl_page = 'internal_page_max=16K'
        base_create = 'key_format=S,value_format=S,' + intl_page
        self.pr("create sys")
        self.session.create(self.uri, base_create)
        conf = \
          ',tiered_storage=(auth_token=%s,' % self.auth_token + \
          'bucket=%s,' % self.bucket1 + \
          'bucket_prefix=%s,' % self.prefix1 + \
          'local_retention=%d,' % self.retention1 + \
          'name=%s,' % self.ss_name + \
          'object_target_size=%s)' % self.object_uri
        self.pr("create non-sys tiered")
        self.session.create(self.uri1, base_create + conf)
        conf = ',tiered_storage=(name=none)'
        self.pr("create non tiered/local")
        self.session.create(self.uri_none, base_create + conf)

        c = self.session.open_cursor(self.uri)
        c1 = self.session.open_cursor(self.uri1)
        cn = self.session.open_cursor(self.uri_none)
        c["0"] = "0"
        c1["0"] = "0"
        cn["0"] = "0"
        self.check(c, 1)
        self.check(c1, 1)
        self.check(cn, 1)
        c.close()

        flush = 0
        # Check the local retention. After a flush_tier call the object file should exist in
        # the local database. Then after sleeping long enough it should be removed.
        self.pr("flush tier no checkpoint")
        self.session.flush_tier(None)
        flush += 1
        # We should not have flushed either tiered table.
        skip = self.get_stat(stat.conn.flush_tier_skipped, None)
        self.assertEqual(skip, 2)

        self.session.checkpoint()
        self.session.flush_tier(None)
        # Now we should have switched both tables. The skip value should stay the same.
        skip = self.get_stat(stat.conn.flush_tier_skipped, None)
        self.assertEqual(skip, 2)
        switch = self.get_stat(stat.conn.flush_tier_switched, None)
        self.assertEqual(switch, 2)
        flush += 1
        self.pr("Check for ")
        self.pr(self.obj1file)
        self.assertTrue(os.path.exists(self.obj1file))
        self.assertTrue(os.path.exists(self.obj2file))
        time.sleep(self.retention + 1)
        # We call flush_tier here because otherwise the internal thread that
        # processes the work units won't run for a while. This call will signal
        # the internal thread to process the work units.
        self.session.flush_tier('force=true')
        flush += 1
        # We still sleep to give the internal thread a chance to run. Some slower
        # systems can fail here if we don't give them time.
        time.sleep(1)
        self.pr("Check removal of ")
        self.pr(self.obj1file)
        self.assertFalse(os.path.exists(self.obj1file))

        c = self.session.open_cursor(self.uri)
        c["1"] = "1"
        c1["1"] = "1"
        cn["1"] = "1"
        self.check(c, 2)
        c.close()

        c = self.session.open_cursor(self.uri)
        c["2"] = "2"
        c1["2"] = "2"
        cn["2"] = "2"
        self.check(c, 3)
        c1.close()
        cn.close()
        self.session.checkpoint()

        self.pr("flush tier again, holding open cursor")
        self.session.flush_tier(None)
        flush += 1

        c["3"] = "3"
        self.check(c, 4)
        c.close()

        calls = self.get_stat(stat.conn.flush_tier, None)
        self.assertEqual(calls, flush)
        obj = self.get_stat(stat.conn.tiered_object_size, None)
        self.assertEqual(obj, self.object_sys_val)

        # As we flush each object, the next object exists, but our first flush was a no-op.
        # So the value for the last file: object should be 'flush'.
        last = 'last=' + str(flush)
        # For now all earlier objects exist. So it is always 1 until garbage collection
        # starts removing them.
        oldest = 'oldest=1'
        fileuri = self.fileuri_base + str(flush) + '.wtobj'
        self.check_metadata(self.tiereduri, intl_page)
        self.check_metadata(self.tiereduri, last)
        self.check_metadata(self.tiereduri, oldest)
        self.check_metadata(fileuri, intl_page)
        self.check_metadata(self.objuri, intl_page)

        #self.pr("verify stats")
        # Verify the table settings.
        #obj = self.get_stat(stat.dsrc.tiered_object_size, self.uri)
        #self.assertEqual(obj, self.object_sys_val)
        #obj = self.get_stat(stat.dsrc.tiered_object_size, self.uri1)
        #self.assertEqual(obj, self.object_uri_val)
        #obj = self.get_stat(stat.dsrc.tiered_object_size, self.uri_none)
        #self.assertEqual(obj, 0)

        #retain = self.get_stat(stat.dsrc.tiered_retention, self.uri)
        #self.assertEqual(retain, self.retention)
        #retain = self.get_stat(stat.dsrc.tiered_retention, self.uri1)
        #self.assertEqual(retain, self.retention1)
        #retain = self.get_stat(stat.dsrc.tiered_retention, self.uri_none)
        #self.assertEqual(retain, 0)

        # Now test some connection statistics with operations.
        retain = self.get_stat(stat.conn.tiered_retention, None)
        self.assertEqual(retain, self.retention)
        self.session.flush_tier(None)
        skip1 = self.get_stat(stat.conn.flush_tier_skipped, None)
        switch1 = self.get_stat(stat.conn.flush_tier_switched, None)
        # Make sure the last checkpoint and this flush tier are timed differently
        # so that we can specifically check the statistics and code paths in the test.
        # Sleep some to control the execution.
        time.sleep(2)
        self.session.flush_tier('force=true')
        skip2 = self.get_stat(stat.conn.flush_tier_skipped, None)
        switch2 = self.get_stat(stat.conn.flush_tier_switched, None)
        self.assertGreater(switch2, switch1)

        self.assertEqual(skip1, skip2)
        flush += 2
        calls = self.get_stat(stat.conn.flush_tier, None)
        self.assertEqual(calls, flush)

        # Test reconfiguration.
        config = 'tiered_storage=(local_retention=%d)' % self.retention1
        self.pr("reconfigure")
        self.conn.reconfigure(config)
        retain = self.get_stat(stat.conn.tiered_retention, None)
        self.assertEqual(retain, self.retention1)

        # Call flush_tier with its various configuration arguments. It is difficult
        # to force a timeout or lock contention with a unit test. So just test the
        # call for now.
        #
        # There have been no data changes nor checkpoints since the last flush_tier with
        # force, above. The skip statistics should increase and the switched
        # statistics should stay the same.
        skip1 = self.get_stat(stat.conn.flush_tier_skipped, None)
        switch1 = self.get_stat(stat.conn.flush_tier_switched, None)
        self.session.flush_tier('timeout=100')
        skip2 = self.get_stat(stat.conn.flush_tier_skipped, None)
        switch2 = self.get_stat(stat.conn.flush_tier_switched, None)
        self.assertEqual(switch1, switch2)
        self.assertGreater(skip2, skip1)

        self.session.flush_tier('lock_wait=false')
        self.session.flush_tier('sync=off')
        flush += 3
        self.pr("reconfigure get stat")
        calls = self.get_stat(stat.conn.flush_tier, None)
        self.assertEqual(calls, flush)

        # Test that the checkpoint and flush times work across a connection restart.
        # Make modifications and then close the connection (which will checkpoint).
        # Reopen the connection and call flush_tier. Verify this flushes the object.
        c = self.session.open_cursor(self.uri)
        c["4"] = "4"
        self.check(c, 5)
        c.close()
        # Manually reopen the connection because the default function above tries to
        # make the bucket directories.
        self.reopen_conn(config = self.saved_conn)
        skip1 = self.get_stat(stat.conn.flush_tier_skipped, None)
        switch1 = self.get_stat(stat.conn.flush_tier_switched, None)
        self.session.flush_tier(None)
        skip2 = self.get_stat(stat.conn.flush_tier_skipped, None)
        switch2 = self.get_stat(stat.conn.flush_tier_switched, None)
        #
        # Due to the above modification, we should skip the 'other' table while
        # switching the main tiered table. Therefore, both the skip and switch
        # values should increase by one.
        self.assertEqual(skip2, skip1 + 1)
        self.assertEqual(switch2, switch1 + 1)

if __name__ == '__main__':
    wttest.run()
