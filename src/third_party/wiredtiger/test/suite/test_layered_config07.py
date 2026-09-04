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

# Note that the APIs we are testing are not meant to be used directly
# by any WiredTiger application, these APIs are used internally.
# However, it is useful to do tests of this API independently.

class test_layered_config07(wttest.WiredTigerTestCase, DisaggConfigMixin):

    disagg_storages = gen_disagg_storages(disagg_only = True)

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(disagg_storages)

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_disagg_basic(self):
        # Test some basic functionality of the page log API. The methods left out are either
        # deprecated, or exercised extensively by other tests.
        session = self.session
        page_log = self.conn.get_page_log('palite')

        ckpt_complete_args = wiredtiger.PageLogCompleteCheckpointArgs()
        ckpt_complete_args.checkpoint_id = 1
        ckpt_complete_args.checkpoint_timestamp = 0
        ckpt_complete_args.checkpoint_metadata = 'Checkpoint'
        ckpt_complete_args.lsn = 0
        ckpt_complete_args.checkpoint_oldest_timestamp = 0
        page_log.pl_complete_checkpoint(session, ckpt_complete_args)
        ckpt1_lsn = ckpt_complete_args.lsn

        # The checkpoint just written is the only one, so it is also the most recent.
        (lsn, _, ts, meta) = page_log.pl_get_complete_checkpoint(session)
        self.assertEqual((lsn, ts, meta), (ckpt1_lsn, 0, 'Checkpoint'))

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
        page20_full_lsn = put_args_main.lsn
        put_args_delta.base_lsn = page20_full_lsn
        put_args_delta.backlink_lsn = page20_full_lsn
        handle.plh_put(session, 20, 2, put_args_delta, page20_delta1)
        page20_delta1_lsn = put_args_delta.lsn

        put_args_main.backlink_lsn = 0
        put_args_main.base_lsn = 0
        handle.plh_put(session, 21, 2, put_args_main, page21_full)
        page21_full_lsn = put_args_main.lsn
        put_args_delta.base_lsn = page21_full_lsn
        put_args_delta.backlink_lsn = page21_full_lsn
        handle.plh_put(session, 21, 2, put_args_delta, page21_delta1)
        page21_delta1_lsn = put_args_delta.lsn

        put_args_delta.base_lsn = page20_full_lsn
        put_args_delta.backlink_lsn = page20_delta1_lsn
        handle.plh_put(session, 20, 2, put_args_delta, page20_delta2)

        get_args = wiredtiger.PageLogGetArgs()
        get_args.lsn = put_args_delta.lsn
        page20_results = handle.plh_get(session, 20, 2, get_args)

        get_args.lsn = page21_delta1_lsn
        page21_results = handle.plh_get(session, 21, 2, get_args)

        self.assertEqual(page20_results, [page20_full, page20_delta1, page20_delta2])
        self.assertEqual(page21_results, [page21_full, page21_delta1])

        discard_args = wiredtiger.PageLogDiscardArgs()
        discard_args.flags = 0
        discard_args.base_lsn = page20_full_lsn
        discard_args.backlink_lsn = put_args_delta.lsn
        handle.plh_discard(session, 20, 2, discard_args)

        self.assertGreater(discard_args.lsn, put_args_delta.lsn)

        # The last LSN is the one the discard just took.
        (_, last_lsn) = page_log.pl_get_last_lsn(session)
        self.assertEqual(last_lsn, discard_args.lsn)

        # The page ids live for this table at a given LSN. Both pages are live before the
        # discard; at the discard's own LSN page 20 is gone and only page 21 remains.
        self.assertEqual(sorted(handle.plh_get_page_ids(session, page21_delta1_lsn)), [20, 21])
        self.assertEqual(sorted(handle.plh_get_page_ids(session, discard_args.lsn)), [21])

        # A second checkpoint, so the selector below has more than one to choose between.
        ckpt_complete_args.checkpoint_id = 2
        ckpt_complete_args.checkpoint_timestamp = 5
        ckpt_complete_args.checkpoint_metadata = 'Checkpoint2'
        ckpt_complete_args.lsn = 0
        page_log.pl_complete_checkpoint(session, ckpt_complete_args)
        ckpt2_lsn = ckpt_complete_args.lsn
        self.assertGreater(ckpt2_lsn, ckpt1_lsn)

        # No selector and an explicit zero both ask for the most recent checkpoint.
        for selector in ((), (0,)):
            (lsn, _, ts, meta) = page_log.pl_get_complete_checkpoint(session, *selector)
            self.assertEqual((lsn, ts, meta), (ckpt2_lsn, 5, 'Checkpoint2'))

        # A selector naming a checkpoint returns that one; a selector between two checkpoints
        # returns the next one above it; one above the newest finds nothing.
        self.assertEqual(page_log.pl_get_complete_checkpoint(session, ckpt1_lsn)[3], 'Checkpoint')
        self.assertEqual(page_log.pl_get_complete_checkpoint(session, ckpt1_lsn + 1)[0], ckpt2_lsn)
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: page_log.pl_get_complete_checkpoint(session, ckpt2_lsn + 1), '/WT_NOTFOUND/')

        # Abandoning drops everything written above the newest complete checkpoint: the page
        # written after it goes, the checkpoint itself and the pages below it stay.
        put_args_main.backlink_lsn = 0
        put_args_main.base_lsn = 0
        handle.plh_put(session, 22, 2, put_args_main, encode_bytes('Hello22'))
        page22_lsn = put_args_main.lsn
        self.assertEqual(sorted(handle.plh_get_page_ids(session, page22_lsn)), [21, 22])

        page_log.pl_abandon_checkpoint(session)
        self.assertEqual(sorted(handle.plh_get_page_ids(session, page22_lsn)), [21])
        self.assertEqual(page_log.pl_get_complete_checkpoint(session)[0], ckpt2_lsn)

        page_log.terminate(session) # dereference
