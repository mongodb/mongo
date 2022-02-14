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
# test_config08.py
#   Test the configuration that enables/disables dirty table flushing.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

class test_config08(wttest.WiredTigerTestCase):

    logging = [
        ('log_off', dict(logging='false')),
        ('log_on', dict(logging='true')),
    ]
    flush = [
        ('flush_off', dict(flush='false')),
        ('flush_on', dict(flush='true')),
    ]

    scenarios = make_scenarios(logging, flush)

    # This test varies the logging and file flush settings and therefore needs to set up its own
    # connection config. Override the standard method.
    def conn_config(self):
        return 'create,log=(enabled={}),file_close_sync={}'.\
            format(self.logging,self.flush)

    def test_config08(self):
        # Create a table with logging setting matching the connection level config.
        table_params = 'key_format=i,value_format=S,log=(enabled={})'.format(self.logging)
        self.uri = 'table:config_test'

        # Create a table with some data.
        self.session.create(self.uri, table_params)
        c = self.session.open_cursor(self.uri, None)
        c[0] = 'ABC' * 4096
        c[1] = 'DEF' * 4096
        c[2] = 'GHI' * 4096
        c.close()

        # API calls that require exclusive file handles should return EBUSY if file_close_sync is
        # set to false and logging is disabled.
        if self.logging == 'false' and self.flush == 'false':
            # WT won't allow this operation as exclsuive file handle is not possible
            # with modified table.
            self.assertTrue(self.raisesBusy(lambda: self.session.verify(self.uri, None)),
                "was expecting API call to fail with EBUSY")

            # Taking a checkopoint should make WT happy.
            self.session.checkpoint()
            self.session.verify(self.uri, None)
        else:
            # All other combinations of configs should not return EBUSY.
            self.session.verify(self.uri, None)

        # This will catch a bug if we return EBUSY from final shutdown.
        self.conn.close()

if __name__ == '__main__':
    wttest.run()
