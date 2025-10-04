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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtbackup import backup_base


# test_live_restore05.py
# Reproduce a live restore edge case that resulted in duplicate metadata entries.
class test_live_restore05(backup_base):
    format_values = [
        ('row_integer', dict(key_format='i', value_format='S')),
        ('column_store', dict(key_format='r', value_format='S'))
    ]
    scenarios = make_scenarios(format_values)
    nrows = 10
    ntables = 1

    def test_live_restore05(self):
        # Live restore is not supported on Windows.
        if os.name == 'nt':
            self.skipTest('Unix specific test skipped on Windows')

        # Create a folder to save the wt utility output.
        util_out_path = 'UTIL'
        os.mkdir(util_out_path)

        uris = []
        for i in range(self.ntables):
            uri = f'file:collection-{i}'
            uris.append(uri)
            ds = SimpleDataSet(self, uri, self.nrows, key_format=self.key_format,
                               value_format=self.value_format)
            ds.populate()

        self.session.checkpoint()

        # Close the default connection.
        os.mkdir("SOURCE")
        self.take_full_backup("SOURCE")
        self.close_conn()

        # Remove everything but SOURCE / stderr / stdout / util output folder.
        for f in glob.glob("*"):
            if not f == "SOURCE" and not f == "UTIL" and not f == "stderr.txt" and not f == "stdout.txt":
                os.remove(f)

        # Open a live restore connection with no background migration threads to leave it in an
        # unfinished state.
        self.close_conn()
        self.open_conn(config="log=(enabled),statistics=(all),live_restore=(enabled=true,path=\"SOURCE\",threads_max=0)")

        self.runWt(['-l', 'SOURCE', 'dump', "file:WiredTiger.wt"],
            outfilename="WiredTiger_out.txt",
            reopensession=False,
            failure=False)

        # If we find duplicate live restore entries, crash.
        with open('WiredTiger_out.txt', 'r') as file:
            for line in file:
                index = line.find("live_restore=")
                if index != -1:
                    new_line = line[index+len("live_restore="):]
                    assert(new_line.find("live_restore=") == -1)
