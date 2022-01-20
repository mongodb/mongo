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
# test_bug028.py
#   Test buffer alignment and direct I/O settings.

import os
import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

class test_bug028(wttest.WiredTigerTestCase, suite_subprocess):
    format_values = [
        ('fix', dict(key_format = 'r', value_format='8t')),
        ('row', dict(key_format = 'S', value_format='S')),
        ('var', dict(key_format = 'r', value_format='S')),
    ]
    ckpt_directio = [
        ('ckpt-directio', dict(ckpt_directio=True)),
        ('no-ckpt-directio', dict(ckpt_directio=False)),
    ]
    data_directio = [
        ('data-directio', dict(data_directio=True)),
        ('no-data-directio', dict(data_directio=False)),
    ]
    log_directio = [
        ('log-directio', dict(log_directio=True)),
        ('no-log-directio', dict(log_directio=False)),
    ]
    reopen = [
        ('in-memory', dict(reopen=False)),
        ('on-disk', dict(reopen=True)),
    ]
    scenarios = make_scenarios(format_values, ckpt_directio, data_directio, log_directio, reopen)

    # Test is opening its own home.
    def setUpConnectionOpen(self, dir):
        return None
    def setUpSessionOpen(self, conn):
        return None

    # Open a connection with direct I/O and a non-standard buffer alignment.
    def open_conn(self, run, align, fail):
        # This is a smoke test of buffer alignment and direct I/O. We don't want to debug any
        # of this under any systems that aren't vanilla POSIX. Do not try and fix this test on
        # those systems, just stop the test from running there.
        if os.name != 'posix':
            self.skipTest('skipping buffer alignment and direct I/O test on non-POSIX system')

        config = 'create'
        config += ",buffer_alignment=" + align
        config += ',direct_io=('
        if self.ckpt_directio:
            config += ',checkpoint'
        if self.data_directio:
            config += ',data'
        if self.log_directio:
            config += ',log'
            self.skipTest('FIXME WT-8684 skipping test of logging with direct I/O')
        config += ')'
        if self.log_directio:
            config += ',log=(enabled=true)'

        homedir = 'test_bug028.' + str(run)
        os.mkdir(homedir)
        self.pr(homedir)
        if fail:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: wiredtiger.wiredtiger_open(homedir, config),
                '/memory allocation .* failed/')
            return
        else:
            self.conn = wiredtiger.wiredtiger_open(homedir, config)
        self.session = self.conn.open_session(self.session_config)

        uri = 'table:buf_align'
        nitems = 50000 # Enough items to create multiple pages.

        # Configuring direct I/O with checkpoint or data files forces allocation and page sizes to
        # match the buffer alignment size. Else, we have to do it explicitly.
        size = align
        if align == '-1':
            size = '4K'
        dsconfig = 'allocation_size={}'.format(size)
        if not self.ckpt_directio and not self.data_directio:
            dsconfig += ',internal_page_max={},leaf_page_max={}'.format(size, size)
        ds = SimpleDataSet(self, uri, nitems,
            key_format=self.key_format, value_format=self.value_format, config=dsconfig)
        ds.populate()

        # Optionally flush the file to disk and re-open it.
        if self.reopen:
            self.conn.close()
            self.conn = wiredtiger.wiredtiger_open(homedir, config)
            self.session = self.conn.open_session(self.session_config)

        c = self.session.open_cursor(uri, None, None)
        i = 0
        while True:
            ret = c.next()
            if ret != 0:
                break
            c.get_key()
            c.get_value()
            i += 1
        self.assertEqual(i, nitems)
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

        self.conn.close()

    # Open a connection with a non-standard buffer alignment.
    def test_bug028(self):
        self.open_conn(1, '-1', False)
        self.open_conn(2, '1K', False)
        self.open_conn(3, '2K', False)
        self.open_conn(4, '4K', False)
        self.open_conn(5, '32K', False)
        self.open_conn(6, '64K', False)
        self.open_conn(7, '8000', True)

if __name__ == '__main__':
    wttest.run()
