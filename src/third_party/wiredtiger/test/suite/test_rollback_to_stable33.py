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

from rollback_to_stable_util import verify_rts_logs
import wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# Smoke test RTS on in-memory databases.
class test_rollback_to_stable33(wttest.WiredTigerTestCase):
    format_values = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    logged = [
        ('no', dict(logged=False)),
        ('yes', dict(logged=True))
    ]
    scenarios = make_scenarios(format_values, logged)

    # Configure an in-memory database.
    conn_config = 'in_memory=true,verbose=(rts:5)'

    # Don't raise errors for these, the expectation is that the RTS verifier will
    # run on the test output.
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')
        self.addTearDownAction(verify_rts_logs)

    # Smoke test RTS on in-memory databases.
    def test_rollback_to_stable33(self):
        uri = "table:rollback_to_stable33"
        ds_config = ',log=(enabled=true)' if self.logged else ',log=(enabled=false)'
        ds = SimpleDataSet(self, uri, 500,
            key_format=self.key_format, value_format=self.value_format, config=ds_config)
        ds.populate()

        # Make changes at timestamp 30.
        c = self.session.open_cursor(uri, None, None)
        self.session.begin_transaction()
        c[ds.key(10)] = ds.value(100)
        c[ds.key(11)] = ds.value(101)
        c[ds.key(12)] = ds.value(102)
        self.session.commit_transaction('commit_timestamp=30')
        c.close()

        # Set stable to 20 and rollback.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.conn.rollback_to_stable()

        # Objects with logging enabled should not be rolled back, objects without logging enabled
        # should have their updates rolled back.
        c = self.session.open_cursor(uri, None)
        if self.logged:
            self.assertEquals(c[ds.key(10)], ds.value(100))
            self.assertEquals(c[ds.key(11)], ds.value(101))
            self.assertEquals(c[ds.key(12)], ds.value(102))
        else:
            self.assertEquals(c[ds.key(10)], ds.value(10))
            self.assertEquals(c[ds.key(11)], ds.value(11))
            self.assertEquals(c[ds.key(12)], ds.value(12))

if __name__ == '__main__':
    wttest.run()
