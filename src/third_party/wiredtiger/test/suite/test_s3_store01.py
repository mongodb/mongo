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

import datetime, random, os, wiredtiger, wttest
FileSystem = wiredtiger.FileSystem  # easy access to constants

# test_s3_store01.py
#   Test minimal S3 extension with basic interactions with AWS S3CrtClient.
class test_s3_store01(wttest.WiredTigerTestCase):
    # Generates a unique prefix to be used with the object keys, eg:
    # "s3test_artefacts/python_2022-31-01-16-34-10_623843294/"
    prefix = 's3test_artefacts/python_'
    prefix += datetime.datetime.now().strftime('%Y-%m-%d-%H-%M-%S')
    # Range upto int32_max, matches that of C++'s std::default_random_engine
    prefix += '_' + str(random.randrange(1,2147483646))
    prefix += "/"

    fs_config = 'prefix=' + prefix

    # Bucket name can be overridden by an environment variable.
    bucket_name = os.getenv('WT_S3_EXT_BUCKET')
    if bucket_name is None:
        bucket_name = "s3testext"

    # Load the s3 store extension, skip the test if missing.
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('storage_sources', 's3_store=(config=\"(verbose=-3)\")')

    def get_s3_storage_source(self):
        return self.conn.get_storage_source('s3_store')

    def test_local_basic(self):
        # Test some basic functionality of the storage source API, calling
        # each supported method in the API at least once.
        cache_prefix = "cache-"
        filename = "foobar"
        object_name = "foobar"
    
        session = self.session
        s3_store = self.get_s3_storage_source()
        fs = s3_store.ss_customize_file_system(session, self.bucket_name, "Secret", self.fs_config)
       
        # Test flush functionality and flushing to cache and checking if file exists.
        f = open(filename, 'wb')
        outbytes = ('MORE THAN ENOUGH DATA\n'*100000).encode()
        f.write(outbytes)
        f.close()

        s3_store.ss_flush(session, fs, filename, object_name)
        s3_store.ss_flush_finish(session, fs, filename, object_name)
        self.assertTrue(fs.fs_exist(session, filename))

        fh = fs.fs_open_file(session, filename, FileSystem.open_file_type_data, FileSystem.open_readonly)
        inbytes = bytes(1000000)         # An empty buffer with a million zero bytes.
        fh.fh_read(session, 0, inbytes)  # Read into the buffer.
        self.assertEquals(outbytes[0:1000000], inbytes)
        fh.close(session)

        # Checking that the file still exists in S3 after removing it from the cache.
        os.remove(cache_prefix + self.bucket_name + '/' + filename)
        self.assertTrue(fs.fs_exist(session, filename))

        file_list = [self.prefix + object_name]
        self.assertEquals(fs.fs_directory_list(session, None, None), file_list)

        fs2 = s3_store.ss_customize_file_system(session, self.bucket_name, "Secret", self.fs_config)
        self.assertEquals(fs.fs_directory_list(session, None, None), file_list)

        s3_store.terminate(session)

if __name__ == '__main__':
    wttest.run()
