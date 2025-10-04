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

import os, glob, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet
from wtbackup import backup_base

# test_live_restore03.py
# Test that live_restore->fs_size can returns a valid size when the file only exists in the source
# directory.
# Note: The block_size statistic corresponds to underlying file size.
class test_live_restore03(backup_base):
    nrows = 100

    def test_live_restore03(self):
        # Live restore is not supported on Windows.
        if os.name == 'nt':
            self.skipTest('Unix specific test skipped on Windows')

        uris = ['file:foo', 'table:bar']
        # Create a data set with a 1 file and 1 table data source type.
        for uri in uris:
            ds = SimpleDataSet(self, uri, self.nrows,
            key_format='i', value_format='S')
            ds.populate()

        self.session.checkpoint()

        # Close the default connection.
        os.mkdir("SOURCE")
        self.take_full_backup("SOURCE")
        self.close_conn()

        # Remove everything but SOURCE / stderr / stdout.
        for f in glob.glob("*"):
            if not f == "SOURCE" and not f == "stderr.txt" and not f == "stdout.txt":
                os.remove(f)

        # Open a connection with no live restore background threads to avoid opening file handles
        # in the background.
        os.mkdir("DEST")
        self.open_conn("DEST",config="statistics=(all),live_restore=(enabled=true,path=\"SOURCE\",threads_max=0)")

        # Query the data source block size statistic.
        for uri in uris:
            cursor = self.session.open_cursor(f'statistics:{uri}', None, 'statistics=(size)')
            size = cursor[stat.dsrc.block_size][2]
            self.assertGreater(size, 0)
