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
from wtscenario import make_scenarios

# Verify prefetch for in-memory databases is not allowed.
class test_prefetch03(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'file:{test_name}'

    prefetch_config = 'prefetch=(available=true,default=true)'
    verbose_config = 'verbose=(prefetch:1)'

    config_options = [
        ('default', dict(conn_cfg=f'{prefetch_config},{verbose_config}')),
        ('inmem', dict(conn_cfg=f'in_memory=true,{prefetch_config},{verbose_config}')),
    ]

    scenarios = make_scenarios(config_options)

    def test_prefetch03(self):
        # The in-memory configuration is incompatible with prefetch, so prefetch should be disabled
        # and a message should be logged if the in-memory configuration is set.
        if 'in_memory=true' not in self.conn_cfg:
            self.reopen_conn(".", self.conn_cfg)
        else:
            with self.expectedStdoutPattern('prefetch configuration is incompatible with in-memory configuration'):
                self.reopen_conn(".", self.conn_cfg)
