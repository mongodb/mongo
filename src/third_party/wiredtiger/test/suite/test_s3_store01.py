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

import os, wiredtiger, wttest
FileSystem = wiredtiger.FileSystem  # easy access to constants

# test_s3_store01.py
#   Test minimal S3 extension with basic interactions with AWS S3CrtClient.
class test_s3_store01(wttest.WiredTigerTestCase):
    # Load the s3 store extension, skip the test if missing.
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('storage_sources', 's3_store=(config=\"(verbose=-3)\")')

    def get_s3_storage_source(self):
        return self.conn.get_storage_source('s3_store')

    def test_local_basic(self):
        # Test some basic functionality of the storage source API, calling
        # each supported method in the API at least once.
        session = self.session
        s3_store = self.get_s3_storage_source()

        fs = s3_store.ss_customize_file_system(session, "./objects", "Secret", None)
        fs.terminate(session)

if __name__ == '__main__':
    wttest.run()
