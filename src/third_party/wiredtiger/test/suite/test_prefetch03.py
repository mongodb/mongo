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

# test_prefetch03.py
# Verify prefetch for in-memory databases is not allowed.
class test_prefetch03(wttest.WiredTigerTestCase):
    uri = 'file:test_prefetch03'

    config_options = [
        ('default', dict(conn_cfg='prefetch=(available=true,default=true)')),
        ('inmem', dict(conn_cfg='in_memory=true,prefetch=(available=true,default=true)')),
    ]

    scenarios = make_scenarios(config_options)

    def test_prefetch03(self):
        if 'in_memory=true' not in self.conn_cfg:
            self.reopen_conn(".", self.conn_cfg)
        else:
            with self.expectedStderrPattern('prefetch configuration is incompatible with in-memory configuration'):
                self.assertRaisesException(wiredtiger.WiredTigerError,
                    lambda: self.reopen_conn(".", self.conn_cfg))
