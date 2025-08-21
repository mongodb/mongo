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

import wiredtiger, wttest
from wtscenario import make_scenarios

# test_prepare30.py
# Test that validates that prepare transaction API raises correct errors with preserve_prepared config on

class test_prepare30(wttest.WiredTigerTestCase):

    conn_config_values = [
        ('preserve_prepared_on', dict(expected_error=True, conn_config='precise_checkpoint=true,preserve_prepared=true')),
    ]

    scenarios = make_scenarios(conn_config_values)

    def test_prepare30(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(50))
        uri = 'table:test_prepare31'
        create_params = 'key_format=i,value_format=S'
        self.session.create(uri, create_params)
        ts = 100
        self.session.begin_transaction()

        if self.expected_error:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(ts)),
                        '/prepared_id need to be set if the preserve_prepared config is enabled/')
