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
# test_cursor08.py
# Log cursors with compression
#

from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
import wttest

class test_cursor08(wttest.WiredTigerTestCase, suite_subprocess):
    logmax = "100K"
    tablename = 'test_cursor08'
    uri = 'table:' + tablename
    nkeys = 500

    reopens = [
        ('regular', dict(reopen=False)),
        ('reopen', dict(reopen=True))
    ]
    compress = [
        ('nop', dict(compress='nop')),
        ('snappy', dict(compress='snappy')),
        ('zlib', dict(compress='zlib')),
        ('none', dict(compress='none')),
    ]
    scenarios = make_scenarios(reopens, compress)
    # Load the compression extension, and enable it for logging.
    def conn_config(self):
        return 'log=(enabled,file_max=%s,' % self.logmax + \
            'compressor=%s,remove=false),' % self.compress + \
            'transaction_sync="(method=dsync,enabled)"'

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('compressors', self.compress)

    def test_log_cursor(self):
        # print "Creating %s with config '%s'" % (self.uri, self.create_params)
        create_params = 'key_format=i,value_format=S'
        self.session.create(self.uri, create_params)
        c = self.session.open_cursor(self.uri, None)

        # A binary value.
        value = '\u0001\u0002abcd\u0003\u0004'

        self.session.begin_transaction()
        for k in range(self.nkeys):
            c[k] = value
        self.session.commit_transaction()
        c.close()

        if self.reopen:
            self.reopen_conn()

        # Check for these values via a log cursor
        c = self.session.open_cursor("log:", None)
        count = 0
        while c.next() == 0:
            # lsn.file, lsn.offset, opcount
            keys = c.get_key()
            # txnid, rectype, optype, fileid, logrec_key, logrec_value
            values = c.get_value()
            # We are only looking for log records that have a key/value
            # pair.
            if values[4] != b'':
                if value.encode() in values[5]:     # logrec_value
                    count += 1
        c.close()
        self.assertEqual(count, self.nkeys)

if __name__ == '__main__':
    wttest.run()
