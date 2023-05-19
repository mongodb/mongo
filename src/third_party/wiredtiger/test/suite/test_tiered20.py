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
# test_tiered20.py
#    Test to check for conflicts in writing to the cloud.

from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtscenario import make_scenarios
import errno, filecmp, os, time, wiredtiger, wttest

# Return a boolean string that is acceptable for WT configuration.
def wt_boolean(b):
    if b:
        return "true"
    else:
        return "false"

class test_tiered20(TieredConfigMixin, wttest.WiredTigerTestCase):
    tiered_storage_sources = gen_tiered_storage_sources()
    scenarios = make_scenarios(tiered_storage_sources)

    def additional_conn_config(self):
        # For tiered scenarios, we are asking for a short local retention (local files are removed promptly),
        # and to not fail if we get a flush error.  A failure in this sense would result in an assertion,
        # which crashes the test suite.  Better that we continue, and we can detect that the flush
        # actually failed by other means.
        if self.is_tiered_scenario():
            return 'tiered_storage=(local_retention=0),debug_mode=(tiered_flush_error_continue=true)'
        else:
            return ''

    def create_flush_drop(self, uri, remove_shared):
        self.session.create(uri, "key_format=S,value_format=S")
        c = self.session.open_cursor(uri)
        c["a"] = "a"
        c["b"] = "b"

        c.close()
        if self.is_tiered_scenario():
            # Do a regular checkpoint first, technically shouldn't be needed?
            self.session.checkpoint()
            self.session.checkpoint('flush_tier=(enabled,force=true)')
            self.session.drop(uri, "remove_files=true,remove_shared={}".format(wt_boolean(remove_shared)))
        else:
            self.session.drop(uri, "force=true")

    def test_tiered_overwrite(self):
        uri_a = "table:tiereda"
        uri_b = "table:tieredb"
        uri_b_local_file1 = 'tieredb-0000000001.wtobj'

        # We should be able to do this test for any tiered scenario, not just dir_store.
        # Remove this 'if' and comment when FIXME-WT-11004 is finished.
        if self.is_tiered_scenario() and not self.is_local_storage:
            self.skipTest('Some storage sources do not yet guard against overwrite.')

        # For any scenario, we should be able to fully drop and then recreate a table.
        for i in range(0, 3):
            self.create_flush_drop(uri_a, True)

        # For tiered scenarios only, we have specific tests that deal with files that
        # have been shared to the bucket.
        if not self.is_tiered_scenario():
            self.skipTest('Tiered storage is required for this test.')

        # We should be able to drop locally (not removing shared), and then
        # we should get an error if we create again.  Having an error protects us
        # from clobbering objects in a bucket that might apply to a previous test
        # or run.

        # For this test, we don't clean up what's in the cloud, only clean up locally.
        self.create_flush_drop(uri_a, False)

        # Now, we're creating the same URI.  Even though there should be no local
        # files or metadata, it should detect existing items in the cloud.
        # We expect the tiered create will return an EEXIST, with no output message.
        expected_errno = os.strerror(errno.EEXIST)
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.create_flush_drop(uri_a, False), expected_errno)

        # Now, create another WT home directory and link the buckets together.
        # This emulates multiple systems sharing the same AWS bucket.
        # We want to make sure that there is no way to have a collision when
        # tiered tables are created, even when they are of the same name and prefix.
        #
        # In a production setup, multiple systems should never have the same prefix,
        # but we don't want to rely on that.  Once an object is written to a bucket
        # it should never be overwritten.  It can only be removed (and yes, then
        # the name would be available in the storage)
        os.mkdir('SECOND')
        session1 = self.session  # save the first session
        conn2 = self.setUpConnectionOpen('SECOND')
        session2 = self.setUpSessionOpen(conn2)

        if self.is_local_storage:   # dir_store
            os.rmdir('SECOND/bucket1')
            os.symlink('../bucket1', 'SECOND/bucket1')

        # Create URIs on each connection (as if on two systems).
        # This should not conflict, as nothing has been pushed to the shared storage.
        session1.create(uri_b, "key_format=S,value_format=S")
        session2.create(uri_b, "key_format=S,value_format=S")
        c1 = session1.open_cursor(uri_b)
        c1["a"] = "a"
        c1.close()
        c2 = session2.open_cursor(uri_b)
        c2["a"] = "SOMETHING_VERY_DIFFERENT!" * 1000
        c2.close()
        session1.checkpoint()
        session2.checkpoint()

        # Make sure the file systems in the first connection.
        self.assertTrue(os.path.exists(uri_b_local_file1))

        # The first flush from the first "system" should succeed
        session1.checkpoint('flush_tier=(enabled,force=true)')
        if self.is_local_storage:   # dir_store
            uri_b_shared_file1 = os.path.join(self.bucket, self.bucket_prefix + uri_b_local_file1)
            uri_b_second_file1 = os.path.join('SECOND', uri_b_local_file1)

            self.assertTrue(os.path.exists(uri_b_shared_file1))
            self.assertTrue(filecmp.cmp(uri_b_local_file1, uri_b_shared_file1))

        # The second flush from the other "system" should detect the conflict.
        # Normally such a failure would crash Python, but we've changed our
        # configuration such that we continue after the fail to write.
        # We'll check for the error message we expect in the error output.
        with self.expectedStderrPattern(expected_errno):
            session2.checkpoint('flush_tier=(enabled,force=true)')

        # At this point, in dir_store, we can verify that the original copy
        # is still in place, and that the version from the second directory
        # did not overwrite it.
        if self.is_local_storage:   # dir_store
            self.assertTrue(os.path.exists(uri_b_shared_file1))
            self.assertFalse(filecmp.cmp(uri_b_second_file1, uri_b_shared_file1))
            self.assertTrue(filecmp.cmp(uri_b_local_file1, uri_b_shared_file1))

        # We're done with the second connection.
        session2.close()
        conn2.close()

        # We'd like to enable the following code to make sure the local file gets removed.
        # Removing the local file guarantees that we'd go to the cloud to get the data.
        # However, this loop appears to last forever, it seems to keep the
        # file around because the file handle is in use.  Even inserting a
        # self.reopen_conn() into the loop does not fix it.  FIXME-WT-10995.
        #
        #    # Make sure the local file is removed, so we actually will go to the
        #    # cloud the next time this URI is accessed.
        #    while os.path.exists(uri_b_local_file1):
        #        self.pr('sleeping...')
        #        time.sleep(1)
        #    self.pr('{}: file is removed, continuing'.format(uri_b_local_file1))

        # Meanwhile, the first system should not have any trouble accessing
        # the data via the cloud.
        self.reopen_conn()
        c1 = self.session.open_cursor(uri_b)
        self.assertEqual(c1["a"], "a")
        c1.close()

if __name__ == '__main__':
    wttest.run()
