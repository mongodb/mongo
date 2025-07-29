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

import errno, inspect, os, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, gen_disagg_storages
from wtscenario import make_scenarios

def encode_bytes(str):
    return bytes(str, 'utf-8')

# test_disagg01.py
# Note that the APIs we are testing are not meant to be used directly
# by any WiredTiger application, these APIs are used internally.
# However, it is useful to do tests of this API independently.

class test_disagg01(wttest.WiredTigerTestCase, DisaggConfigMixin):

    disagg_storages = gen_disagg_storages('test_disagg01', disagg_only = True)

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages)

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        DisaggConfigMixin.conn_extensions(self, extlist)

    def breakpoint(self):
        import pdb, sys
        sys.stdin = open('/dev/tty', 'r')
        sys.stdout = open('/dev/tty', 'w')
        sys.stderr = open('/dev/tty', 'w')
        pdb.set_trace()

    def check_package(self, page_log, package, values):
        i = 0
        while True:
            returns = page_log.pl_get_package_part(self.session, package, i)
            if len(returns) == 0:
                break
            self.assertEquals(returns, values[i])
            i += 1
        self.assertEquals(len(values), i)

    def test_disagg_basic(self):
        # Test some basic functionality of the page log API, calling
        # each supported method in the API at least once.
        self.skipTest('Disagg test is broken until PageLogPutArgs, PageLogGetArgs work')
        session = self.session
        page_log = self.conn.get_page_log('palm')

        page_log.pl_begin_checkpoint(session, 1)
        page_log.pl_complete_checkpoint(session, 1)

        page_log.pl_begin_checkpoint(session, 2)
        handle = page_log.pl_open_handle(session, 1)

        page20_full = encode_bytes('Hello20')
        page20_delta1 = encode_bytes('Delta20-1')
        page20_delta2 = encode_bytes('Delta20-2')
        page21_full = encode_bytes('Hello21')
        page21_delta1 = encode_bytes('Delta21-1')

        flags_main = 0x0
        flags_delta = wiredtiger.WT_PAGE_LOG_DELTA

        put_args_main = wiredtiger.PageLogPutArgs()
        put_args_main.flags = flags_main
        put_args_delta = wiredtiger.PageLogPutArgs()
        put_args_delta.flags = flags_delta

        handle.plh_put(session, 20, 2, put_args_main, page20_full)
        handle.plh_put(session, 20, 2, put_args_delta, page20_delta1)
        handle.plh_put(session, 21, 2, put_args_main, page21_full)
        handle.plh_put(session, 21, 2, put_args_delta, page21_delta1)
        handle.plh_put(session, 20, 2, put_args_delta, page20_delta2)

        get_args = wiredtiger.PageLogGetArgs()

        page20_results = handle.plh_get(session, 20, 2, get_args)
        page21_results = handle.plh_get(session, 21, 2, get_args)

        self.assertEquals(page20_results, [page20_full, page20_delta1, page20_delta2])
        self.assertEquals(page21_results, [page21_full, page21_delta1])

        page_log.terminate(session)
