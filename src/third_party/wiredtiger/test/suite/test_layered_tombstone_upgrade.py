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

# test_layered_tombstone_upgrade.py
#   The stable tombstone encoding mode is detected from the data, not configured: at checkpoint
#   pickup a node adopts the mode indicated by the checkpoint metadata's compatible_version, a
#   checkpoint predating the unescaped format (or the version fields entirely) is legacy escaped
#   data, and a database starting from empty shared storage is unescaped. The
#   disaggregated.legacy_tombstone_encoding_break_glass option is a break-glass override that
#   forces the mode, winning over detection with a warning rather than an error.

import os, re
import wiredtiger, wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

class tombstone_upgrade_base(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,'
    collide = b'\x14\x14ab'  # colliding value: stored differently by each mode
    control = b'plain'       # control value: stored identically by both modes

    def setUp(self):
        super().setUp()
        self.ignoreStdoutPattern('stable table value in the tombstone namespace')

    # A follower in automatic mode by default; a mode forces it via the break-glass override.
    def follower_config(self, forced=None):
        enc = '' if forced is None else f'legacy_tombstone_encoding_break_glass={forced},'
        return self.extensionsConfig() + ',create,' + self.conn_base_config + \
            f'disaggregated=({enc}role="follower")'

    # Leader writes a colliding value and a control value, then checkpoints; the
    # completed-checkpoint metadata's compatible_version records its mode.
    def leader_checkpoint(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.session.create(self.uri, 'key_format=i,value_format=u')
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        c[1] = self.collide
        c[2] = self.control
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()

    # Read both keys back through a layered cursor and confirm they decode to their original bytes,
    # proving the adopted mode matches how the data was written.
    def assert_reads(self, conn, collide):
        s = conn.open_session()
        rc = s.open_cursor(self.uri)
        s.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        rc.set_key(1)
        self.assertEqual(rc.search(), 0)
        self.assertEqual(rc.get_value(), collide)
        rc.set_key(2)
        self.assertEqual(rc.search(), 0)
        self.assertEqual(rc.get_value(), self.control)
        s.rollback_transaction()
        rc.close()
        s.close()

    def conn_stat(self, conn, stat_id):
        s = conn.open_session()
        c = s.open_cursor('statistics:')
        value = c[stat_id][2]
        c.close()
        s.close()
        return value

    # The statistic recording the mode: 0 not yet determined, 1 escaped, 2 unescaped.
    def encoding_stat(self, conn):
        return self.conn_stat(conn, stat.conn.disagg_stable_tombstone_encoding)

    # Assert the checkpoint version statistics: the binary's capability descriptors are
    # compile-time constants, and the storage fields are the most recently picked-up checkpoint's
    # (0 before any pickup).
    def assert_version_stats(self, conn, storage_version, storage_compat):
        self.assertEqual(self.conn_stat(conn, stat.conn.disagg_checkpoint_binary_version), 2)
        self.assertEqual(
            self.conn_stat(conn, stat.conn.disagg_checkpoint_binary_compatible_version), 2)
        self.assertEqual(
            self.conn_stat(conn, stat.conn.disagg_checkpoint_storage_version), storage_version)
        self.assertEqual(
            self.conn_stat(conn, stat.conn.disagg_checkpoint_storage_compatible_version),
            storage_compat)

@disagg_test_class
class test_layered_tombstone_upgrade_escaped(tombstone_upgrade_base):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # The escaped format only arises on legacy data; fabricate it with the break-glass override.
    def conn_config(self):
        return self.extensionsConfig() + ',create,' + self.conn_base_config + \
            'disaggregated=(legacy_tombstone_encoding_break_glass=true,role="leader")'

    def test_adopts_escaped_mode(self):
        # A follower in automatic mode adopts the escaped mode indicated by the checkpoint's
        # compatible_version and round-trips the colliding value written in that mode. Escaped
        # checkpoints stay readable by every version, so compatible_version stays at 1.
        self.leader_checkpoint()
        meta = self.disagg_get_complete_checkpoint_meta()
        self.assertIn('compatible_version=1', meta)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        # No checkpoint has been picked up yet, so the mode is not yet determined.
        self.assertEqual(self.encoding_stat(conn_follow), 0)
        self.assert_version_stats(conn_follow, 0, 0)
        self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.encoding_stat(conn_follow), 1)
        self.assert_version_stats(conn_follow, 2, 1)
        self.assert_reads(conn_follow, self.collide)
        conn_follow.close()

    def test_adopts_unversioned_checkpoint_as_escaped(self):
        # A checkpoint written before the metadata version fields existed carries neither version
        # nor compatible_version; the reader defaults both to 1, which predates the unescaped
        # format, so the checkpoint means legacy escaped data and must be adopted as such.
        self.leader_checkpoint()
        meta = self.disagg_get_complete_checkpoint_meta()
        unversioned = re.sub(r',?(compatible_)?version=\d+', '', meta)
        self.assertNotIn('version=', unversioned)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{unversioned}")')
        self.assertEqual(self.encoding_stat(conn_follow), 1)
        self.assert_reads(conn_follow, self.collide)
        conn_follow.close()

    def test_leader_cold_start_adopts_escaped(self):
        # A leader restarted with no local files discovers the latest complete checkpoint itself
        # while opening, rather than through a reconfigure, and a leader in automatic mode must
        # adopt the escaped mode indicated there before serving reads.
        self.leader_checkpoint()
        self.restart_without_local_files(config=self.conn_base_config +
            'disaggregated=(role="leader")')
        self.assertEqual(self.encoding_stat(self.conn), 1)
        self.assert_reads(self.conn, self.collide)
        # The restarted connection has no stable timestamp and the precise shutdown checkpoint
        # needs one.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20))

    def test_forced_unescaped_overrides_detection(self):
        # The break-glass override wins over detection: the pickup succeeds with a warning, not an
        # error, and the forced mode stays in effect. The forced mode disagrees with how the data
        # was written, so only the control key is read back.
        self.leader_checkpoint()
        conn_follow = self.wiredtiger_open('follower', self.follower_config('false'))
        with self.expectedStdoutPattern('forced off by configuration, overriding'):
            self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.encoding_stat(conn_follow), 2)

        s = conn_follow.open_session()
        rc = s.open_cursor(self.uri)
        s.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        rc.set_key(2)
        self.assertEqual(rc.search(), 0)
        self.assertEqual(rc.get_value(), self.control)
        s.rollback_transaction()
        rc.close()
        s.close()
        conn_follow.close()

@disagg_test_class
class test_layered_tombstone_upgrade_new_database(tombstone_upgrade_base):
    test_name = __qualname__
    uri = f'layered:{test_name}'
    stable_uri = f'file:{test_name}.wt_stable'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # Automatic mode throughout: starting from empty shared storage adopts the unescaped format.
    def conn_config(self):
        return self.extensionsConfig() + ',create,' + self.conn_base_config + \
            'disaggregated=(role="leader")'

    def test_new_database_unescaped(self):
        self.leader_checkpoint()
        self.assertEqual(self.encoding_stat(self.conn), 2)
        # The unescaped format is recorded by raising the minimum reader version.
        meta = self.disagg_get_complete_checkpoint_meta()
        self.assertIn('compatible_version=2', meta)

        # The colliding value is stored raw: reading the stable constituent directly, bypassing the
        # layered decode, returns the original bytes with no escape byte appended.
        c = self.session.open_cursor(self.stable_uri)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        c.set_key(1)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), self.collide)
        self.session.rollback_transaction()
        c.close()

        # A follower in automatic mode adopts the unescaped mode and round-trips the value.
        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.encoding_stat(conn_follow), 2)
        self.assert_version_stats(conn_follow, 2, 2)
        self.assert_reads(conn_follow, self.collide)
        conn_follow.close()

    def test_forced_escaped_overrides_detection(self):
        # The break-glass override in the other direction: a follower forced to the escaped mode
        # picks up an unescaped checkpoint with a warning and keeps the forced mode.
        self.leader_checkpoint()
        conn_follow = self.wiredtiger_open('follower', self.follower_config('true'))
        with self.expectedStdoutPattern('forced on by configuration, overriding'):
            self.disagg_advance_checkpoint(conn_follow)
        self.assertEqual(self.encoding_stat(conn_follow), 1)

        s = conn_follow.open_session()
        rc = s.open_cursor(self.uri)
        s.begin_transaction('read_timestamp=' + self.timestamp_str(20))
        rc.set_key(2)
        self.assertEqual(rc.search(), 0)
        self.assertEqual(rc.get_value(), self.control)
        s.rollback_transaction()
        rc.close()
        s.close()
        conn_follow.close()

    def test_reconfigure_rejects_the_option(self):
        # The break-glass option is not part of the reconfigure schema, so the mode cannot change
        # for the life of the connection: flipping it on a running node would mix escaped and
        # unescaped values in one data set.
        self.leader_checkpoint()
        with self.expectedStderrPattern('unknown configuration key'):
            with self.assertRaises(wiredtiger.WiredTigerError):
                self.conn.reconfigure(
                    'disaggregated=(legacy_tombstone_encoding_break_glass=false)')
        self.assert_reads(self.conn, self.collide)

@disagg_test_class
class test_layered_tombstone_upgrade_flip_panics(tombstone_upgrade_base, suite_subprocess):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    # Automatic mode throughout: the leader's checkpoints carry the unescaped-format
    # compatible_version.
    def conn_config(self):
        return self.extensionsConfig() + ',create,' + self.conn_base_config + \
            'disaggregated=(role="leader")'

    def subprocess_flip_after_adoption_panics(self):
        # Subprocess body: a follower in automatic mode adopts escaped from a checkpoint stripped
        # of its version fields, then picks up the same checkpoint with its original
        # compatible_version=2 metadata, flipping the adopted mode. Expected to panic.
        self.leader_checkpoint()
        meta = self.disagg_get_complete_checkpoint_meta()
        unversioned = re.sub(r',?(compatible_)?version=\d+', '', meta)
        self.assertNotIn('version=', unversioned)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{unversioned}")')
        self.assertEqual(self.encoding_stat(conn_follow), 1)
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{meta}")')

    def test_flip_after_adoption_panics(self):
        # The shared storage changing its stable tombstone encoding under a live node is a
        # data-integrity hazard: a pickup whose compatible_version indicates a different mode after
        # one was adopted must panic rather than adopt the new mode. Panic aborts, so the flip runs
        # in a subprocess.
        [returncode, home] = self.run_subprocess_function(
            'SUBPROCESS_flip_after_adoption_panics',
            f'test_layered_tombstone_upgrade.{self.test_name}.subprocess_flip_after_adoption_panics',
            silent=True)
        self.assertNotEqual(returncode, 0,
            'Expected subprocess to panic on a stable tombstone encoding flip')
        self.check_file_contains(os.path.join(home, 'stderr.txt'),
            'stable tombstone encoding changed from on to off')
        # This connection wrote nothing but the precise shutdown checkpoint still needs a stable
        # timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

    def subprocess_flip_to_escaped_panics(self):
        # Subprocess body, the reverse direction: the follower adopts the unescaped mode from the
        # checkpoint's original metadata, then picks up the same checkpoint stripped of its version
        # fields, which derives as legacy escaped. Expected to panic.
        self.leader_checkpoint()
        meta = self.disagg_get_complete_checkpoint_meta()
        unversioned = re.sub(r',?(compatible_)?version=\d+', '', meta)
        self.assertNotIn('version=', unversioned)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{meta}")')
        self.assertEqual(self.encoding_stat(conn_follow), 2)
        conn_follow.reconfigure(f'disaggregated=(checkpoint_meta="{unversioned}")')

    def test_flip_to_escaped_after_adoption_panics(self):
        # A node that adopted the unescaped format and is handed a legacy checkpoint must panic
        # just like the opposite direction.
        [returncode, home] = self.run_subprocess_function(
            'SUBPROCESS_flip_to_escaped_panics',
            f'test_layered_tombstone_upgrade.{self.test_name}.subprocess_flip_to_escaped_panics',
            silent=True)
        self.assertNotEqual(returncode, 0,
            'Expected subprocess to panic on a stable tombstone encoding flip')
        self.check_file_contains(os.path.join(home, 'stderr.txt'),
            'stable tombstone encoding changed from off to on')
        # This connection wrote nothing but the precise shutdown checkpoint still needs a stable
        # timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))

@disagg_test_class
class test_layered_tombstone_upgrade_verbose(tombstone_upgrade_base):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + ',create,' + self.conn_base_config + \
            'disaggregated=(role="leader")'

    def test_adoption_logged_at_info_verbosity(self):
        # The adoption line is the diagnostic record of the mode in effect and must be emitted,
        # at INFO so it stays out of default-verbosity output.
        self.leader_checkpoint()
        with self.expectedStdoutPattern(
                r'stable tombstone encoding off \(the checkpoint compatible version\)'):
            self.restart_without_local_files(config=self.conn_base_config +
                'verbose=[disaggregated_storage:0],disaggregated=(role="leader")')
        self.assertEqual(self.encoding_stat(self.conn), 2)
        self.assert_reads(self.conn, self.collide)
        # The restarted connection has no stable timestamp and the precise shutdown checkpoint
        # needs one.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(20))
