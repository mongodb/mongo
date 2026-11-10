#!/usr/bin/env python3
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
# A follower steps up after a rolled-back prepared remove or writes separated by a truncate.
# Timestamped reads stay correct through checkpoint and restart. Removes are written directly
# into ingest to exercise the drain independently of the layered cursor.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_stepup15(wttest.WiredTigerTestCase):
    table = __qualname__
    uri = f'layered:{table}'
    ingest_uri = f'file:{table}.wt_ingest'
    skip_msg = 'step-up drain skipped'
    read_ts = (6, 9, 15, 22, 28)

    disagg_storages = gen_disagg_storages(disagg_only=True)
    workloads = [
        ('rollback_then_remove', dict(found_at=[], orphans=1, rollback_before_remove=True,
            insert_after_rollback=False, truncate_before_remove=False)),
        ('rollback_insert_remove', dict(found_at=[15], orphans=0, rollback_before_remove=True,
            insert_after_rollback=True, truncate_before_remove=False)),
        ('insert_before_truncate', dict(found_at=[15], orphans=0, rollback_before_remove=False,
            insert_after_rollback=False, truncate_before_remove=True)),
    ]
    scenarios = make_scenarios(disagg_storages, workloads)

    def conn_config(self):
        config = self.extensionsConfig() + ',create,statistics=(all),disaggregated=(role="leader")'
        if self.rollback_before_remove:
            config += ',preserve_prepared=true,precise_checkpoint=true'
        return config

    def orphan_stat(self):
        c = self.session.open_cursor('statistics:')
        val = c[wiredtiger.stat.conn.disagg_step_up_ingest_drain_orphan_remove][2]
        c.close()
        return val

    def search(self, uri, ts):
        if ts is not None:
            self.session.begin_transaction('read_timestamp=' + self.timestamp_str(ts))
        c = self.session.open_cursor(uri)
        c.set_key('k')
        ret = c.search()
        value = c.get_value() if ret == 0 else None
        c.close()
        if ts is not None:
            self.session.rollback_transaction()
        return ret, value

    def check_reads(self):
        for ts in self.read_ts:
            ret, value = self.search(self.uri, ts)
            if ts in self.found_at:
                self.assertEqual((ret, value), (0, b'v'))
            else:
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)

    def test_stepup_remove(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c['other'] = b'v'
        if self.truncate_before_remove:
            c['a'] = b'v'
            c['b'] = b'v'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(5))
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.conn.reconfigure('disaggregated=(role="follower")')
        if self.rollback_before_remove:
            prepared = self.conn.open_session()
            c = prepared.open_cursor(self.ingest_uri)
            prepared.begin_transaction()
            c['k'] = b'\x14\x14'
            c.close()
            prepared.prepare_transaction('prepare_timestamp=' + self.timestamp_str(12) +
                ',prepared_id=' + self.prepared_id_str(1))
            prepared.rollback_transaction('rollback_timestamp=' + self.timestamp_str(13))
            prepared.close()
            if self.insert_after_rollback:
                c = self.session.open_cursor(self.ingest_uri)
                self.session.begin_transaction()
                c['k'] = b'v'
                self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(14))
                c.close()
        if self.truncate_before_remove:
            c = self.session.open_cursor(self.ingest_uri)
            self.session.begin_transaction()
            c['k'] = b'v'
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(12))
            c.close()
            start = self.session.open_cursor(self.uri)
            stop = self.session.open_cursor(self.uri)
            start.set_key('a')
            stop.set_key('b')
            self.session.begin_transaction()
            self.session.truncate(None, start, stop, None)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(15))
            start.close()
            stop.close()
        c = self.session.open_cursor(self.ingest_uri)
        self.session.begin_transaction()
        c['k'] = b'\x14\x14'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        c.close()

        # Let checkpoints include the removes while readers can still read before them.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(30))

        warning = f'{self.uri}: step-up drain skipped orphan removes for {self.orphans} key(s) ' \
            'with no value in the stable table'
        before = self.orphan_stat()
        self.cleanStdout()
        self.conn.reconfigure('disaggregated=(role="leader")')
        output = self.readStdout()
        self.cleanStdout()

        # One warning for the affected table, naming the table and count.
        self.assertEqual(output.count(self.skip_msg), 1 if self.orphans else 0)
        if self.orphans:
            self.assertEqual(output.count(warning), 1)
        self.assertEqual(self.orphan_stat() - before, self.orphans)

        # Reads at every timestamp: after the step-up, after the stable pages are written, and
        # after a restart.
        self.check_reads()
        self.session.checkpoint()
        self.check_reads()
        self.reopen_conn()
        self.check_reads()
