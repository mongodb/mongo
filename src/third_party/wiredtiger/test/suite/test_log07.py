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

import wttest
from suite_subprocess import suite_subprocess

# Verify that 'wt printlog' correctly renders WT_LOGREC_SYSTEM records.
class test_log07(wttest.WiredTigerTestCase, suite_subprocess):
    # A small file_max forces log file switches, each of which writes a
    # prev_lsn system record.
    conn_config = 'log=(enabled,file_max=100K)'
    uri = 'table:log07'

    # Read the whole printlog output; check_file_contains only looks at the
    # first 100K, which may not reach the records we care about.
    def read_printlog(self, args):
        self.runWt(args, outfilename='printlog.out')
        with open('printlog.out') as f:
            return f.read()

    def test_printlog_system_record(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Open an incremental backup cursor and checkpoint so that a backup_id
        # system record is written in addition to the prev_lsn records.
        bkup = self.session.open_cursor('backup:', None,
            'incremental=(enabled,granularity=1M,this_id="ID1")')
        self.session.checkpoint()
        bkup.close()

        # Write enough data to roll over several log files, forcing prev_lsn
        # system records to be written.
        c = self.session.open_cursor(self.uri)
        bigval = 'V' * 1000
        for i in range(2000):
            c[str(i)] = bigval
        c.close()
        self.session.checkpoint()

        out = self.read_printlog(['printlog'])

        # The system record header must be present, along with its embedded
        # operations. If the WT_LOGREC_SYSTEM case fails to parse the record,
        # these operations are not printed.
        self.assertTrue('"type" : "system"' in out,
            'printlog did not emit a system record header')
        self.assertTrue('"optype": "prev_lsn"' in out,
            'printlog did not emit a prev_lsn operation')
        self.assertTrue('"optype": "backup_id"' in out,
            'printlog did not emit a backup_id operation')
