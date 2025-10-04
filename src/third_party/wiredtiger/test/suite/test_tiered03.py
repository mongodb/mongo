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

import os, re
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources, get_conn_config
import wtscenario, wttest
from wtdataset import SimpleDataSet

# test_tiered03.py
#    Test block-log-structured tree configuration options.
class test_tiered03(wttest.WiredTigerTestCase, TieredConfigMixin):
    K = 1024
    M = 1024 * K
    G = 1024 * M
    # TODO: tiered: change this to a table: URI, otherwise we are
    # not using tiered files.  The use of a second directory for
    # sharing would probably need to be reworked.
    uri = 'file:test_tiered03'

    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered03', tiered_only=True)

    # Occasionally add a lot of records to vary the amount of work flush does.
    record_count_scenarios = wtscenario.quick_scenarios(
        'nrecs', [10, 10000], [0.9, 0.1])
    scenarios = wtscenario.make_scenarios(storage_sources, record_count_scenarios,\
         prune=100, prunelong=500)

    absolute_bucket_dir = None  # initialied in conn_config to an absolute path

    def conn_config(self):
        bucket_ret = self.bucket

        # The bucket format for the S3 store is the name and the region separated by a semi-colon.
        if self.ss_name == 's3_store':
            cache_dir = self.bucket[:self.bucket.find(';')] + '-cache'
        else:
            cache_dir = self.bucket + '-cache'

        # We have multiple connections that want to share a bucket.
        # For the directory store, the first time this function is called, we'll
        # establish the absolute path for the bucket, and always use that for
        # the bucket name.
        # The cache directory name is a relative one, so it won't be shared
        # between connections.
        if self.ss_name == 'dir_store':
            if self.absolute_bucket_dir == None:
                self.absolute_bucket_dir = os.path.join(os.getcwd(), self.bucket)
                os.mkdir(self.absolute_bucket_dir)
            bucket_ret = self.absolute_bucket_dir
        return get_conn_config(self) + 'cache_directory=%s)' % cache_dir

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    # Test sharing data between a primary and a secondary
    def test_sharing(self):
        self.skipTest('Sharing the checkpoint file containing transaction ids is not supported')

        ds = SimpleDataSet(self, self.uri, 10)
        ds.populate()
        ds.check()
        self.session.checkpoint()
        ds.check()

        # Create a secondary database
        dir2 = os.path.join(self.home, 'SECONDARY')
        os.mkdir(dir2)
        conn2 = self.setUpConnectionOpen(dir2)
        session2 = conn2.open_session()

        # Reference the tree from the secondary:
        metac = self.session.open_cursor('metadata:')
        metac2 = session2.open_cursor('metadata:', None, 'readonly=0')
        uri2 = self.uri[:5] + '../' + self.uri[5:]
        metac2[uri2] = metac[self.uri] + ",readonly=1"

        cursor2 = session2.open_cursor(uri2)
        ds.check_cursor(cursor2)
        cursor2.close()

        newds = SimpleDataSet(self, self.uri, 10000)
        newds.populate()
        newds.check()
        self.session.checkpoint()
        newds.check()

        # Check we can still read from the last checkpoint
        cursor2 = session2.open_cursor(uri2)
        ds.check_cursor(cursor2)
        cursor2.close()

        # Bump to new checkpoint
        origmeta = metac[self.uri]
        checkpoint = re.search(r',checkpoint=\(.+?\)\)', origmeta).group(0)[1:]
        self.pr('Orig checkpoint: ' + checkpoint)
        session2.alter(uri2, checkpoint)
        self.pr('New metadata on secondaery: ' + metac2[uri2])

        # Check that we can see the new data
        cursor2 = session2.open_cursor(uri2)
        newds.check_cursor(cursor2)
