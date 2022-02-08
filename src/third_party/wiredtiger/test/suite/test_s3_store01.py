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

# test_s3_store01.py
#   Test minimal S3 extension with basic interactions with AWS S3CrtClient.
class test_s3_store01(wttest.WiredTigerTestCase):
    # Temporarily hardcode the bucket name.
    bucket_name = ""

    # Load the s3 store extension, skip the test if missing.
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('storage_sources', 's3_store=(config=\"(verbose=-3)\")')

    def get_s3_storage_source(self):
        return self.conn.get_storage_source('s3_store')

    def test_local_basic(self):
        # Test some basic functionality of the storage source API, calling
        # each supported method in the API at least once.
        bucket_name = "rubysfirstbucket"
        cache_prefix = "cache-"
        filename = "foobar"
        object_name = "foobar"
    
        session = self.session
        s3_store = self.get_s3_storage_source()
        fs = s3_store.ss_customize_file_system(session, bucket_name, "Secret", None)
       
        # Test flush functionality and flushing to cache and checking if file exists.
        f = open(filename, 'wb')
        outbytes = ('Ruby\n'*100).encode()
        f.write(outbytes)
        f.close()

        s3_store.ss_flush(session, fs, filename, object_name)
        s3_store.ss_flush_finish(session, fs, filename, object_name)
        self.assertTrue(fs.fs_exist(session, filename))

        # Checking that the file still exists in S3 after removing it from the cache.
        os.remove(cache_prefix + bucket_name + '/' + filename)
        self.assertTrue(fs.fs_exist(session, filename))

        fs2 = s3_store.ss_customize_file_system(session, "wt-bucket", "Secret", None)
        _ = fs2.fs_directory_list(session, self.bucket_name, '')

        fs.terminate(session)
        fs2.terminate(session)

if __name__ == '__main__':
    wttest.run()
