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

# test_layered_dhandle01.py
# A follower's layered-table ingest btrees can be reclaimed once their data
# is durable in the stable table.
#
# A follower never checkpoints its ingest btrees, so an ingest btree, once
# written to, stays dirty forever. The sweep server now understands that ingest
# btrees with content can be closed if their most recent durable change is older
# than the latest checkpoint (making all content in the ingest redundant relative
# to the matching stable table).

import os, time
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages, Oplog
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_dhandle01(wttest.WiredTigerTestCase):
    conn_base_config = 'create,statistics=(all),' + \
        'file_manager=(close_idle_time=1,close_scan_interval=1,close_handle_minimum=0),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'
    conn_config_follower = conn_base_config + 'disaggregated=(role="follower")'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)
    sweep_timeout = 60

    def open_follower(self):
        return self.wiredtiger_open(
            'follower', self.extensionsConfig() + ',' + self.conn_config_follower)

    # Closed ingest handles stay on the handle list. We sample debug handle flags and
    # derive which bit pattern means "open" without hard-coding a generated WT_DHANDLE_* value.
    def ingest_flags(self, conn):
        self.cleanStdout()
        conn.debug_info('handles')
        dump = self.readStdout(maxchars=2000000)
        self.cleanStdout()
        flags = {}
        name = None
        for line in dump.splitlines():
            stripped = line.strip()
            if stripped.startswith('Name: '):
                name = stripped[len('Name: '):]
                if not name.endswith('.wt_ingest'):
                    name = None
            elif stripped.startswith('Flags:') and name is not None:
                flags[name] = int(stripped.split()[-1], 16)
                name = None
        return flags

    def open_ingest_flag_mask(self, flags):
        # Every sampled ingest handle is open; bits set on all of them identify that state.
        self.assertGreater(len(flags), 0, 'no ingest handles to sample')
        mask = 0xffff
        for value in flags.values():
            mask &= value
        self.assertNotEqual(mask, 0, 'open ingest handles share no flag bits')
        return mask

    def open_ingest_count(self, flags, flag_mask, skip=None):
        count = 0
        for name, value in flags.items():
            if skip is not None and skip in name:
                continue
            if value & flag_mask == flag_mask:
                count += 1
        return count

    def ingest_is_open(self, flags, flag_mask, name):
        return name in flags and flags[name] & flag_mask == flag_mask

    def wait_for_open_ingest_count(self, conn, minimum, skip=None):
        deadline = time.time() + self.sweep_timeout
        while time.time() < deadline:
            flags = self.ingest_flags(conn)
            if len(flags) == 0:
                time.sleep(0.1)
                continue
            open_mask = self.open_ingest_flag_mask(flags)
            if self.open_ingest_count(flags, open_mask, skip) >= minimum:
                return open_mask
            time.sleep(0.1)
        self.fail('expected at least %d open ingest handles on the follower' % minimum)

    def assert_no_ingest_file(self, home, name):
        path = os.path.join(home, name) if home else name
        self.assertFalse(os.path.exists(path), path + ' should not exist')

    # Advance the global checkpoint timestamp by writing to a churn table at
    # successive whole-second timestamps and taking leader checkpoints the follower
    # picks up, until the predicate holds or we run out of patience. The churn must
    # push the checkpoint timestamp past each idle table's max_ingest_write_ts.
    #
    # We step the high 32 bits (the seconds field of a MongoDB <seconds:increment>
    # timestamp) so the checkpoint timestamp crosses second boundaries. That matters
    # because of the reduced-contention way that ingest btrees store timestamps that
    # indicate when they can be closed.
    def churn_until(self, churn_uri, sfollow, conn_follow, predicate):
        for second in range(1, 41):
            ts = (second << 32) + 1
            cl = self.session.open_cursor(churn_uri)
            cf = sfollow.open_cursor(churn_uri)
            with self.transaction(commit_timestamp=ts):
                cl[second] = 'churn'
            with self.transaction(session=sfollow, commit_timestamp=ts):
                cf[second] = 'churn'
            cl.close()
            cf.close()
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
            self.session.checkpoint()
            self.disagg_advance_checkpoint(conn_follow)
            time.sleep(1)
            if predicate():
                return
        self.fail('sweep did not reclaim the ingest btrees')

    def test_ingest_reclaimed(self):
        ntables = 60
        conn_follow = self.open_follower()
        sfollow = conn_follow.open_session('')
        oplog = Oplog()

        tables = []
        for i in range(ntables):
            uri = f'layered:dh01_{i:05d}'
            self.session.create(uri, 'key_format=S,value_format=S')
            sfollow.create(uri, 'key_format=S,value_format=S')
            tables.append((uri, oplog.add_uri(uri)))

        # Write a little to every table, then leave them all idle. Apply to the
        # leader (durable in stable after checkpoint) and to the follower (ingest).
        for (_, t) in tables:
            oplog.insert(t, 5)
        apply_pos = len(oplog._entries)
        oplog.apply(self, self.session, 0, apply_pos)
        oplog.apply(self, sfollow, 0, apply_pos)
        self.conn.set_timestamp(
            f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        # Create does not leave a file behind. The metadata entry is the object.
        self.assert_no_ingest_file('', 'dh01_00000.wt_ingest')
        self.assert_no_ingest_file('follower', 'dh01_00000.wt_ingest')

        # Every table now has an open, dirty ingest btree on the follower.
        open_mask = self.wait_for_open_ingest_count(conn_follow, ntables)

        # Drive the global checkpoint timestamp past every table's
        # max_ingest_write_ts; sweep then discards the now-durable ingest btrees.
        self.session.create('layered:dh01_churn', 'key_format=i,value_format=S')
        sfollow.create('layered:dh01_churn', 'key_format=i,value_format=S')
        self.churn_until('layered:dh01_churn', sfollow, conn_follow,
            lambda: self.open_ingest_count(
                self.ingest_flags(conn_follow), open_mask, 'churn') <= 5)

        # The idle ingest btrees must be reclaimed. The churn table can remain.
        self.assertLessEqual(self.open_ingest_count(
                self.ingest_flags(conn_follow), open_mask, 'churn'), 5,
            'follower ingest btrees were not reclaimed')

        # No data was lost: every key reads back on the follower, served from the
        # stable component now that the ingest btrees are gone.
        oplog.check(self, sfollow, 0, len(oplog._entries))

        sfollow.close()
        conn_follow.close()

    def test_truncate_durable_reclaim(self):
        uri = 'layered:dh01_trunc'
        nrows = 1000
        lo, hi = 100, 700      # truncate removes keys [lo, hi] inclusive

        conn_follow = self.open_follower()
        sfollow = conn_follow.open_session('')

        self.session.create(uri, 'key_format=i,value_format=S')
        sfollow.create(uri, 'key_format=i,value_format=S')

        # Apply identical operations on leader (-> stable) and follower (-> ingest
        # plus the layered dhandle's truncate list): write nrows, then truncate a
        # range. The follower's truncate entry is the state we must not lose when
        # its ingest btree is later discarded.
        def write_and_truncate(session):
            with self.transaction(session=session, commit_timestamp=nrows):
                c = session.open_cursor(uri)
                for i in range(nrows):
                    c[i] = 'value' + str(i)
                c.close()

            c_start = session.open_cursor(uri)
            c_start.set_key(lo)
            c_stop = session.open_cursor(uri)
            c_stop.set_key(hi)

            with self.transaction(session=session, commit_timestamp=nrows + 1):
                session.truncate(None, c_start, c_stop, None)

            c_start.close()
            c_stop.close()

        write_and_truncate(self.session)
        write_and_truncate(sfollow)

        # Make the leader's data and truncate durable, and let the follower pick it
        # up so its own ingest copy becomes redundant.
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(nrows + 1)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        expected = nrows - (hi - lo + 1)   # surviving rows [0,100) and (700,1000)

        def follower_row_count():
            c = sfollow.open_cursor(uri)
            count = 0
            while c.next() == 0:
                count += 1
            c.close()
            return count

        self.assert_no_ingest_file('', 'dh01_trunc.wt_ingest')
        self.assert_no_ingest_file('follower', 'dh01_trunc.wt_ingest')

        # Churn with checkpoints so sweep closes the idle ingest btree.
        ingest = 'file:dh01_trunc.wt_ingest'
        open_mask = self.open_ingest_flag_mask({ingest: self.ingest_flags(conn_follow)[ingest]})
        self.session.create('layered:dh01_churn', 'key_format=i,value_format=S')
        sfollow.create('layered:dh01_churn', 'key_format=i,value_format=S')
        self.churn_until('layered:dh01_churn', sfollow, conn_follow,
            lambda: not self.ingest_is_open(
                self.ingest_flags(conn_follow), open_mask, ingest))

        self.assertFalse(self.ingest_is_open(
                self.ingest_flags(conn_follow), open_mask, ingest),
            'follower ingest btree with a pending truncate was not reclaimed')

        # The truncate must still be reflected after the ingest btree is gone: the
        # range is absent and the rest is present, served from the stable component.
        self.assertEqual(follower_row_count(), expected)

        # Step up: the drain replays the truncate from the layered dhandle's list
        # against stable, with the ingest btree already discarded. Data must remain
        # correct.
        conn_follow.reconfigure('disaggregated=(role="leader")')
        self.assertEqual(follower_row_count(), expected)

        sfollow.close()
        conn_follow.close('debug=(skip_checkpoint=true)')
