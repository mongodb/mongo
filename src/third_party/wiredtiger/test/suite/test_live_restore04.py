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

import filecmp, os, glob, wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wtbackup import backup_base


# test_live_restore04.py
# Test using the wt utility with live restore.
class test_live_restore04(backup_base):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]
    scenarios = make_scenarios(format_values)
    nrows = 10000
    ntables = 3
    conn_config = 'log=(enabled)'

    def get_stat(self, statistic):
        stat_cursor = self.session.open_cursor("statistics:")
        val = stat_cursor[statistic][2]
        stat_cursor.close()
        return val

    def test_live_restore04(self):
        # FIXME-WT-14051: Live restore is not supported on Windows.
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

        # Dump file data for later comparison.
        for i in range(self.ntables):
            dump_out = os.path.join(util_out_path, f'{uris[i]}.out')
            self.runWt(['dump', '-x', uris[i]], outfilename=dump_out)

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
        self.open_conn(config="log=(enabled),statistics=(all),live_restore=(enabled=true,path=\"SOURCE\",threads_max=0)")
        self.close_conn()

        # Check that opening the wt utility without a proper live restore path gives an error.
        lr_dump_out = os.path.join(util_out_path, f'{uris[i]}-error.lr.out')
        self.runWt(['dump', '-x', uris[0]],
                   outfilename=lr_dump_out,
                   errfilename='wterr.txt',
                   reopensession=False,
                   failure=True)
        self.check_non_empty_file('wterr.txt')

        # Check the printlog command works as expected.
        self.runWt(['-l', 'SOURCE', 'printlog'],
            outfilename='printlog.txt',
            reopensession=False,
            failure=False)
        self.check_non_empty_file('printlog.txt')

        # Open a live restore connection through the wt utility. Check that both dump and verify
        # work as expected for each file.
        for i in range(self.ntables):
            lr_dump_out = os.path.join(util_out_path, f'{uris[i]}.lr.out')
            self.runWt(['-l', 'SOURCE', 'dump', '-x', uris[i]],
                       outfilename=lr_dump_out,
                       reopensession=False,
                       failure=False)

            # Check the dump contents is identical to the original dump.
            assert(filecmp.cmp(
                os.path.join(util_out_path, f'{uris[i]}.lr.out'),
                os.path.join(util_out_path, f'{uris[i]}.out')
            ))

            self.runWt(['-l', 'SOURCE', 'verify', uris[i]],
                reopensession=False,
                failure=False)
