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

import re
import wiredtiger
import functools, json, os, shutil, subprocess, wttest
from run import wt_builddir

# These routines help run the various page log sources used by disaggregated storage.
# They are required to manage the generation of disaggregated storage specific configurations.

# Set up configuration
def get_conn_config(disagg_storage):
    if not disagg_storage.is_disagg_scenario():
            return ''
    if disagg_storage.ds_name == 'palite' and not os.path.exists(disagg_storage.bucket):
            os.mkdir(disagg_storage.bucket)
    return \
        f'statistics=(all),name={disagg_storage.ds_name},lose_all_my_data=true'

def gen_disagg_storages(disagg_only = False):
    # Get the string of the configured page_log, e.g. 'palite'.
    page_log = wttest.WiredTigerTestCase.vars().page_log
    page_log_verbose = wttest.WiredTigerTestCase.vars().page_log_verbose
    disagg_storages = [
        (page_log, dict(is_disagg = True,
            is_local_storage = True,
            num_ops=100,
            ds_name = page_log,
            disagg_verbose = int(page_log_verbose))),
        # This must be the last item as we separate the non-disagg from the disagg items later on.
        ('non_disagg', dict(is_disagg = False)),
    ]

    if disagg_only:
        return disagg_storages[:-1]

    return disagg_storages

# For disaggregated test cases, we generally want to ignore verbose warnings about RTS at shutdown.
def disagg_ignore_expected_output(testcase):
    testcase.ignoreStdoutPattern('WT_VERB_RTS')

# Several tests access pages data directly by table id.
# This function computes the shard id for a given table id, which is needed to
# find the right database file in kv_home directory.
# See Storage.NUM_SHARDS in ext/page_log/palite/palite.cpp.
# This must be kept in sync with that value.
NUM_SHARDS = 17

def get_shard_id(table_id):
    return int(table_id) % NUM_SHARDS

# A decorator for a disaggregated test class, that ignores verbose warnings about RTS at shutdown.
# The class decorator takes a class as input, and returns a class to take its place.
def disagg_test_class(cls):
    class disagg_test_case_class(cls, DisaggConfigMixin):
        @functools.wraps(cls, updated=())
        def __init__(self, *args, **kwargs):
            super(disagg_test_case_class, self).__init__(*args, **kwargs)
            disagg_ignore_expected_output(self)

        # Create an early_setup function, only if it hasn't already been overridden.
        if cls.early_setup == wttest.WiredTigerTestCase.early_setup:
            def early_setup(self):
                os.mkdir('follower')
                # Create the home directory for the disagg k/v store, and share it with the follower.
                os.mkdir('kv_home')
                os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

        # Load the page log extension, only if extensions hasn't already been specified.
        if cls.conn_extensions == wttest.WiredTigerTestCase.conn_extensions:
            def conn_extensions(self, extlist):
                self.add_scenario_config()
                return self.disagg_conn_extensions(extlist)

        def page_log(self):
            return wttest.WiredTigerTestCase.vars().page_log

    # Preserve the original name of the wrapped class, so that the test ID is unmodified.
    disagg_test_case_class.__name__ = cls.__name__
    disagg_test_case_class.__qualname__ = cls.__qualname__
    # Preserve the original module, as it is an integral part of the test's identity.
    disagg_test_case_class.__module__ = cls.__module__

    # Put the configured page_log in the config if it isn't already there.
    # The conn_config may be defined as a function, not a string.
    # If so, leave it alone.
    if not hasattr(disagg_test_case_class, 'conn_config'):
        disagg_test_case_class.conn_config = ''
    if type(disagg_test_case_class.conn_config) == str and \
       not 'page_log=' in disagg_test_case_class.conn_config:
        page_log = wttest.WiredTigerTestCase.vars().page_log
        disagg_test_case_class.conn_config += f',disaggregated=(page_log={page_log})'

    return disagg_test_case_class

# This mixin class provides disaggregated storage configuration methods and a few utility functions.
class DisaggConfigMixin:

    # Configuration parameters, can be overridden in test class
    disagg_verbose = 0        # (0 <= level <=3) can be overridden in test class
    disagg_config = None      # a string, can be overridden in test class

    # Internal state tracking
    num_restarts = 0

    # Returns True if the current scenario is disaggregated.
    def is_disagg_scenario(self):
        return hasattr(self, 'is_disagg') and self.is_disagg

    # Setup connection config.
    def conn_config(self):
        return self.disagg_conn_config()

    # Can be overridden
    def additional_conn_config(self):
        return ''

    # Setup disaggregated connection config.
    def disagg_conn_config(self):
        # Handle non_disaggregated storage scenarios.
        if not self.is_disagg_scenario():
            return self.additional_conn_config()

        # Setup directories structure for local disaggregated storage.
        if self.is_local_storage:
            bucket_full_path = os.path.join(self.home, self.bucket)
            if not os.path.exists(bucket_full_path):
                os.mkdir(bucket_full_path)

        # Build disaggregated storage connection string.
        # Any additional configuration appears first to override this configuration.
        return \
            self.additional_conn_config() + f'name={self.ds_name}),'

    # Load the storage sources extension.
    def conn_extensions(self, extlist):
        self.add_scenario_config()
        return self.disagg_conn_extensions(extlist)

    def add_scenario_config(self):
        if not hasattr(self, 'is_disagg'):
            self.is_disagg = True
            self.is_local_storage = False
        if self.is_disagg and not hasattr(self, 'ds_name'):
            self.ds_name = self.vars.page_log

    # Returns configuration to be passed to the extension.
    # Call may override, in which case, they probably want to
    # look at self.is_local_storage or self.ds_name, as every
    # extension has their own configurations that are valid.
    #
    # Some possible values to return: 'verbose=1'
    # or for palite: 'verbose=1,delay_ms=13,force_delay=30'
    # or 'materialization_delay_ms=1000'
    def disaggregated_extension_config(self):
        extension_config = f'verbose={self.disagg_verbose}'

        if self.is_disagg:
            if self.disagg_config:
                extension_config += ',' + self.disagg_config
        return extension_config

    # Load disaggregated storage extension.
    def disagg_conn_extensions(self, extlist):
        # Handle non_disaggregated storage scenarios.
        if not self.is_disagg_scenario():
            return ''

        config = self.disaggregated_extension_config()
        if config == None:
            config = ''
        elif config != '':
            config = f'=(config=\"({config})\")'

        # The page log extension is optional and not all test environments build it.
        if not self.is_local_storage:
            extlist.skip_if_missing = True
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
            extlist.skip_if_missing = True
        extlist.extension('page_log', self.ds_name + config)

    # Get the information about the last completed checkpoint: LSN, ID, timestamp, and metadata
    def disagg_get_complete_checkpoint_ext(self, conn=None):
        if conn is None:
            conn = self.conn
        page_log = conn.get_page_log(self.vars.page_log)

        session = conn.open_session('')
        r = page_log.pl_get_complete_checkpoint(session)
        page_log.terminate(session) # dereference
        session.close()
        return r

    # Get the metadata about the last completed checkpoint
    def disagg_get_complete_checkpoint_meta(self, conn=None):
        (_, _, _, m) = self.disagg_get_complete_checkpoint_ext(conn)
        return m

    # Let the follower pick up the latest checkpoint
    def disagg_advance_checkpoint(self, conn_follower, conn_leader=None):
        m = self.disagg_get_complete_checkpoint_meta(conn_leader)
        conn_follower.reconfigure(f'disaggregated=(checkpoint_meta="{m}")')

    # Switch the leader and the follower
    def disagg_switch_follower_and_leader(self, conn_follower, conn_leader=None):
        if conn_leader is None:
            conn_leader = self.conn

        # Leader step down
        conn_leader.reconfigure(f'disaggregated=(role="follower")')

        meta = self.disagg_get_complete_checkpoint_meta(conn_leader)

        # Follower step up, including picking up the last complete checkpoint
        conn_follower.reconfigure(f'disaggregated=(checkpoint_meta="{meta}",' +\
                                  f'role="leader")')

    def reopen_disagg_conn(self, base_config, directory="."):
        """
        Reopen the connection.
        """
        config = base_config + f'disaggregated=(checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}"),'
        # Step down to avoid shutdown checkpoint
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.close_conn()
        self.open_conn(directory, config)

    def restart_without_local_files(self, config=None, pickup_checkpoint=True, step_up=False):
        """
        Restart the node without local files.
        """

        if pickup_checkpoint:
            # Step down to avoid shutdown checkpoint
            self.conn.reconfigure('disaggregated=(role="follower")')
            checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # Close the current connection
        self.close_conn()

        # Move the local files to another directory
        self.num_restarts += 1
        dir = f'SAVE.{self.num_restarts}'
        os.mkdir(dir)
        for f in os.listdir():
            if os.path.isdir(f):
                continue
            if f.startswith('WiredTiger') or f.endswith('.wt') or f.endswith('.wt_ingest'):
                os.rename(f, os.path.join(dir, f))

        # Also save the PALI database (to aid debugging)
        shutil.copytree('kv_home', os.path.join(dir, 'kv_home'))

        # Reopen the connection
        self.open_conn(config=config)

        # Pick up the last checkpoint
        if pickup_checkpoint:
            self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')

        # Step up as the leader
        if step_up:
            self.conn.reconfigure(f'disaggregated=(role="leader")')

# The oplog simulates the MongoDB oplog and does a bit more.
# Basic operations:
# - Append a bunch of k,v pairs to the oplog. For convenience, this class chooses
#   the keys (string-ized timestamp) and initial values (the same string-ized timestamp).
#   The values may later be changed by update operations.  Timestamp advances on each append.
#
# - Add some update of k,v pairs to the oplog.  From keys already available, some are
#   updated to new values.  Timestamp advances on each update.
#
# - Apply some entries of the oplog to a WT session.  It is assumed that
#   the URIs needed are already created.
#
# - Check the current state of the oplog (up to some position) against tables
#   accessed by a WT session. Both point reads and a read scan are done.
class Oplog(object):
    def __init__(self, key_size=-1, value_size=-1):
        self._timestamp = 0       # last (1-based) timestamp used
        self._entries = []        # list of (table,k,v) triples.
        self._uris = []           # list of (uri,entlist) pairs. Each entlist is an ordered list of
                                  # offsets into _entries that apply to this table.
        self._lookup = dict()     # lookup keyed by (table,k) pairs, gives a list of (ts,v) pairs.
        self._tombstone_value = 'tombstone'

        # For debugging - when _use_timestamps is false, don't actually
        # use the internally generated timestamps with WT calls.
        self._use_timestamps = True
        self.key_size = key_size
        self.value_size = value_size

    def gen_entry(self, i, size):
        """
        Generate the key from an int, by default a simple string.
        The key size can be modified to make this longer.
        """
        result = str(i)
        if size > len(result):
            result += '.' * (size - len(result))
        return result

    # Generate the key
    def gen_key(self, i):
        return self.gen_entry(i, self.key_size)

    # Generate the key integer, the reverse of gen_key.
    def decode_key(self, s):
        if self.key_size <= 0:
            return int(s)
        if '.' in s:
            end = s.index('.')
        else:
            end = len(s)
        result = int(s[:end])
        if self.key_size > end:
            if s[end:] != '.' * (self.key_size - end):
                raise Exception(f'Oplog key returned: {s}, unexpected format for key_size={self.key_size}')
        elif end != len(s):
            raise Exception(f'Oplog key returned: {s}, unexpected format for key_size={self.key_size}')
        return result

    # Generate the value
    # Note: this can be overridden, as long as decode_value is as well
    def gen_value(self, i):
        return self.gen_entry(i, self.value_size)

    def add_uri(self, uri):
        self._uris.append((uri,[]))
        return len(self._uris)     # URI table numbers are 1-based.

    def last_timestamp(self):
        return self._timestamp

    # Get the entlist for a table. The entlist is a list of entries
    # in the oplog (integer 0-based positions) that represent
    # all the operations on a table.
    def _get_entlist(self, table):
        if table <= 0 or table > len(self._uris):
            raise Exception('oplog.append: bad table id')
        return self._uris[table - 1][1]

    # Append a single entry to the oplog, incrementing the internal timestamp.
    # Also update the entlist that is attached to the _uris table, it
    # indicates which oplog entries have activity for a given table -
    # useful for updates.  Also update the _lookup, which gives them
    # current value and history for each key.
    #
    def _append_single(self, table, entlist, k, v):
        self._timestamp += 1
        pos = len(self._entries)
        self._entries.append((table,k,v))
        entlist.append(pos)
        look_key = (table,k)
        look_value = (self._timestamp,v)
        if not look_key in self._lookup:
            # First entry - make a list of ts,value pairs
            self._lookup[look_key] = [look_value]
        else:
            self._lookup[look_key].append(look_value)

    # Get the current value of a key, ignoring timestamps
    def _current_value(self, table, k):
        pairs = self._lookup[(table,k)]
        # the last pair will be the most recent oplog entry
        (_,v) = pairs[-1]
        return v

    # Add inserts to the oplog, return the first entry position
    def insert(self, table, count, start_value=None):
        first_pos = len(self._entries)
        entlist = self._get_entlist(table)

        # Use the timestamp as the key by default, that guarantees these
        # will be inserts, as they've never been seen before.
        ts = self._timestamp + 1
        if start_value == None:
            start_value = ts
        for i in range(start_value, start_value + count):
            self._append_single(table, entlist, i, i)
        return first_pos

    # Update some entries in the oplog for the table
    def update(self, table, count, start_value = 0):
        first_pos = len(self._entries)
        entlist = self._get_entlist(table)

        if len(entlist) == 0:
            # If oplog has no entries for this table,
            # silently succeed
            return
        for i in range(start_value, start_value + count):
            # entindex is the entry we'll update
            entindex = entlist[i]
            (gottable,k,v) = self._entries[entindex]
            if gottable != table:
                raise Exception(f'oplog internal error: intindex for {table} ' + \
                                f'references oplog entry {entindex}, which is not for this table')
            self._append_single(table, entlist, k, v+k)
        return first_pos

    # Remove some entries in the oplog for the table
    def remove(self, table, count, start_value = 0):
        first_pos = len(self._entries)
        entlist = self._get_entlist(table)

        if len(entlist) == 0:
            # If oplog has no entries for this table, silently succeed
            return
        for i in range(start_value, start_value + count):
            # entindex is the entry we'll update
            entindex = entlist[i]
            (gottable,k,_) = self._entries[entindex]
            if gottable != table:
                raise Exception(f'oplog internal error: intindex for {table} ' + \
                                f'references oplog entry {entindex}, which is not for this table')
            self._append_single(table, entlist, k, self._tombstone_value)
        return first_pos

    # Apply the oplog entries starting at the position to the session
    def apply(self, testcase, session, pos, count):
        # Keep a cache of open cursors
        cursors = [None] * len(self._entries)
        while count > 0:
            (table, k, v) = self._entries[pos]
            ts = pos + 1
            cursor = cursors[table - 1]
            if not cursor:
                uri = self._uris[table - 1][0]
                cursor = session.open_cursor(uri)
                cursors[table - 1] = cursor
            session.begin_transaction()
            if v == self._tombstone_value:
                cursor.set_key(self.gen_key(k))
                cursor.remove()
            else:
                cursor[self.gen_key(k)] = self.gen_value(v)
            if self._use_timestamps:
                session.commit_transaction(f'commit_timestamp={testcase.timestamp_str(ts)}')
            else:
                session.commit_transaction()
            pos += 1
            count -= 1
        for cursor in cursors:
            if cursor:
                cursor.close()

    def get_table_snapshot(self, table, pos_limit = -1):
        uri = self._uris[table - 1]
        prev = -1
        result = dict()

        for entindex in uri[1]:
            if entindex < prev:
                raise Exception(f'oplog: entindex for URI {uri[0]} is out of order')
            if pos_limit > 0 and entindex >= pos_limit:
                break

            (_,k,v) = self._entries[entindex]
            if v == self._tombstone_value:
                result.pop(self.gen_key(k), None)  # Remove if exists
            else:
                result[self.gen_key(k)] = self.gen_value(v)

            prev = entindex

        return result

    def check(self, testcase, session, pos, count):
        # Keep a cache of open cursors
        cursors = [None] * len(self._entries)

        pos_limit = pos + count
        # Walk through oplog entries doing point-reads at timestamps
        while count > 0:
            (table, k, v) = self._entries[pos]
            ts = pos + 1
            cursor = cursors[table - 1]
            if not cursor:
                uri = self._uris[table - 1][0]
                cursor = session.open_cursor(uri)
                cursors[table - 1] = cursor
            if self._use_timestamps:
                expected_value_int = v
                session.begin_transaction(f'read_timestamp={testcase.timestamp_str(ts)}')
            else:
                expected_value_int = self._current_value(table, k)
                session.begin_transaction()
            if expected_value_int == self._tombstone_value:
                cursor.set_key(self.gen_key(k))
                testcase.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            else:
                actual_key = self.gen_key(k)
                expected_value = self.gen_value(expected_value_int)
                result_value = cursor[actual_key]
                if (result_value != expected_value):
                    testcase.pr(f'point-read of {actual_key} at ts={ts} gives {result_value}, expected {expected_value}')
                    testcase.assertEqual(result_value, expected_value)
            session.rollback_transaction()
            pos += 1
            count -= 1
        for cursor in cursors:
            if cursor:
                cursor.close()

        # Do a cursor scan, compare against most recent.
        for table, (uri, _) in enumerate(self._uris, 1):
            # Build expected unencoded key/value pairs through pos_limit.
            values = self.get_table_snapshot(table, pos_limit = pos_limit)

            # Walk the cursor and check
            cursor = session.open_cursor(uri)
            for k,v in cursor:
                if not k in values:
                    testcase.pr(f'FAILURE got unexpected key {k}, value {v} from cursor')
                elif v != values[k]:
                    testcase.pr(f'FAILURE at key {k}, got value {v} want {values[k]}')
                testcase.assertEqual(v, values[k])
                del values[k]
            cursor.close()

            testcase.assertEqual(
                values, {}, f'FAILURE URI {uri} missing keys after scan: {sorted(values.keys())}')

    def __str__(self):
        return 'Oplog:' + \
            f' timestamp={self._timestamp}, use_timestamp={self._use_timestamps}' + \
            f' entries - list of (table,k,v)={self._entries},' + \
            f' uris - list of (uri, entlist)={self._uris},' + \
            f' lookup - (table,k) -> list of (ts,value)={self._lookup}'

# DisaggCorruptionMixin provides Python helpers for injecting palite-level page
# corruption into a running disaggregated-storage test by directly mutating the
# per-shard pages_NN.db files.
#
# Palite stores each saved page image as one row in the `pages` table, keyed by
# (table_id, page_id, lsn). A single logical page (table_id, page_id) can have
# many rows: a base image plus successive deltas, forming a delta chain. The
# corruption helpers below operate on one row of that table at a time, i.e.
# they damage a single page image at a specific LSN unless noted otherwise.
#
# Palite holds an exclusive SQLite lock while WT is open, so writes (and the
# multi-shard scan that picks a victim row) must happen while the connection
# is closed. The mutations go through wt_builddir/sqlite3 (not system sqlite3)
# to avoid version skew with the SQLite statically linked into palite.
class DisaggCorruptionMixin:

    # Mirrors WT_PAGE_LOG_DISCARDED in src/include/page_log.h. The value is
    # static-asserted in ext/page_log/palite/palite.cpp.
    WT_PAGE_LOG_DISCARDED = 0x10000

    # ---- Read helpers ----

    def sqlite_select_json(self, table_id, sql_query):
        """Read rows from the pages_NN.db for table_id's shard. Palite uses
        shared SQLite locks for readers, so this is safe whether WT is open
        or closed. Returns a list of dict rows."""
        shard = get_shard_id(table_id)
        db_path = os.path.join(self.home, 'kv_home', f'pages_{shard:02d}.db')
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        result = subprocess.run(
            [sqlite_exe, '-json', db_path, sql_query],
            capture_output=True, text=True, check=True)
        return json.loads(result.stdout) if result.stdout.strip() else []

    def _scan_shards(self, sql, fail_msg):
        """Iterate per-shard pages_NN.db with WT closed and return the first
        non-empty JSON row from any shard, or self.fail() with fail_msg.
        Reopens WT before returning."""
        self.close_conn()
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        try:
            for shard in range(NUM_SHARDS):
                db_path = os.path.join(self.home, 'kv_home', f'pages_{shard:02d}.db')
                if not os.path.exists(db_path):
                    continue
                result = subprocess.run(
                    [sqlite_exe, '-json', db_path, sql],
                    capture_output=True, text=True, check=True)
                if result.stdout.strip():
                    return json.loads(result.stdout)[0]
        finally:
            self.reopen_conn()
        self.fail(fail_msg)

    def _pick_one_row(self, exclude_discarded=False):
        """Pick one row from the palite pages table across all shards.
        If exclude_discarded is set, rows already marked
        WT_PAGE_LOG_DISCARDED are skipped, guaranteeing the returned
        image is still alive. Returns its (table_id, page_id, lsn)
        i.e. one page image."""
        where = (f'WHERE flags & {self.WT_PAGE_LOG_DISCARDED} = 0 '
                 if exclude_discarded else '')
        row = self._scan_shards(
            f'SELECT table_id, page_id, lsn FROM pages {where}'
            'ORDER BY table_id DESC, page_id ASC, lsn DESC LIMIT 1;',
            'no rows found in any pages_NN.db')
        return int(row['table_id']), int(row['page_id']), int(row['lsn'])

    def _pick_multi_lsn_row(self):
        """Pick one logical page (table_id, page_id) whose delta chain
        has >= 2 LSNs in the palite pages table. Returns (table_id,
        page_id)  the per-LSN rows can then be queried separately."""
        row = self._scan_shards(
            'SELECT table_id, page_id, COUNT(*) AS n FROM pages '
            'GROUP BY table_id, page_id HAVING n >= 2 '
            'ORDER BY table_id DESC LIMIT 1;',
            'no rows with >= 2 LSNs found in any pages_NN.db')
        return int(row['table_id']), int(row['page_id'])

    # ---- Write helpers ----

    def _palite_mutate(self, table_id, sql):
        """Run sql against the pages_NN.db for table_id's shard. Returns the
        sqlite3 CLI's stdout split into non-empty lines."""
        shard = get_shard_id(table_id)
        db_path = os.path.join(self.home, 'kv_home', f'pages_{shard:02d}.db')
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        result = subprocess.run(
            [sqlite_exe, '-bail', db_path],
            input=sql, capture_output=True, text=True, check=True)
        return [line for line in result.stdout.splitlines() if line != '']

    def corrupt_checkpoint_metadata_page(self):
        """Overwrite the first byte of the newest shared-metadata page image
        with 0xff so follower checkpoint pickup fails. Returns the
        (table_id, page_id, lsn) of the corrupted image."""
        # Table id 2 / page id 1 are WT_SPECIAL_PALI_TURTLE_FILE_ID and
        # WT_DISAGG_METADATA_MAIN_PAGE_ID: the shared metadata page the follower
        # reads during checkpoint pickup. Corrupting its newest image makes pickup
        # fail while leaving every data-table page intact.
        table_id = 2
        page_id = 1
        # Close before querying so WiredTiger flushes its final checkpoint
        # to palite; the newest lsn we find is then the one pickup will use.
        self.close_conn()
        rows = self.sqlite_select_json(table_id,
            f'SELECT lsn FROM pages WHERE table_id={table_id} '
            f'AND page_id={page_id} ORDER BY lsn DESC LIMIT 1;')
        self.assertTrue(rows,
            f'no metadata page rows for table_id={table_id}, page_id={page_id}')
        lsn = int(rows[0]['lsn'])
        sql = (
            f"UPDATE pages SET page_data = x'ff' || substr(page_data, 2) "
            f"WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};\n"
            f"SELECT changes();\n"
        )
        rows = self._palite_mutate(table_id, sql)
        self._require_one_change(rows, table_id, page_id, lsn)
        return table_id, page_id, lsn

    def corrupt_page_image_at(self, table_id, page_id, lsn):
        """Overwrite the stored data of a specific (table_id, page_id, lsn)
        row in the palite pages table with random bytes."""
        self.close_conn()
        sql = (
            f"UPDATE pages SET page_data = randomblob(length(page_data)) "
            f"WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};\n"
            f"SELECT changes();\n"
        )
        rows = self._palite_mutate(table_id, sql)
        self._require_one_change(rows, table_id, page_id, lsn)
        return table_id, page_id, lsn

    def corrupt_random_page_image(self):
        """Pick one page image (one (page_id, lsn) row in the palite
        pages table) and overwrite the first byte of its stored data
        with 0xff. Other images in the same delta chain are untouched.
        Returns the (table_id, page_id, lsn) of the corrupted image."""
        table_id, page_id, lsn = self._pick_one_row()
        self.close_conn()
        sql = (
            f"UPDATE pages SET page_data = x'ff' || substr(page_data, 2) "
            f"WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};\n"
            f"SELECT changes();\n"
        )
        rows = self._palite_mutate(table_id, sql)
        self._require_one_change(rows, table_id, page_id, lsn)
        return table_id, page_id, lsn

    def delete_random_page_image(self):
        """Pick one page image (one (page_id, lsn) row in the palite
        pages table) and DELETE it. Other images in the same delta
        chain are untouched. Returns the (table_id, page_id, lsn) of
        the deleted image."""
        table_id, page_id, lsn = self._pick_one_row()
        self.close_conn()
        sql = (
            f"DELETE FROM pages "
            f"WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};\n"
            f"SELECT changes();\n"
        )
        rows = self._palite_mutate(table_id, sql)
        self._require_one_change(rows, table_id, page_id, lsn)
        return table_id, page_id, lsn

    def set_random_page_discarded(self):
        """Pick one not-yet-discarded page image (one (page_id, lsn) row
        in the palite pages table) and OR WT_PAGE_LOG_DISCARDED into its
        flags column, marking that single image as a tombstone. Other
        images in the same delta chain are untouched. Returns the
        (table_id, page_id, lsn) of the modified image."""
        table_id, page_id, lsn = self._pick_one_row(exclude_discarded=True)
        self.close_conn()
        sql = (
            f"UPDATE pages SET flags = flags | {self.WT_PAGE_LOG_DISCARDED} "
            f"WHERE table_id={table_id} AND page_id={page_id} AND lsn={lsn};\n"
            f"SELECT changes();\n"
        )
        rows = self._palite_mutate(table_id, sql)
        self._require_one_change(rows, table_id, page_id, lsn)
        return table_id, page_id, lsn

    def truncate_random_delta_chain(self):
        """Pick a logical page (table_id, page_id) whose delta chain has
        >= 2 LSNs and DELETE every image except the base (lowest LSN),
        leaving the base image as the only row for that page. Returns
        (table_id, page_id, kept_lsn, deleted_lsns)."""
        table_id, page_id = self._pick_multi_lsn_row()
        all_lsns = sorted(int(r['lsn']) for r in self.sqlite_select_json(
            table_id,
            f'SELECT lsn FROM pages '
            f'WHERE table_id={table_id} AND page_id={page_id};'))
        kept_lsn = all_lsns[0]
        deleted_lsns = all_lsns[1:]
        self.close_conn()
        sql = (
            f"DELETE FROM pages "
            f"WHERE table_id={table_id} AND page_id={page_id} "
            f"AND lsn != {kept_lsn};\n"
            f"SELECT changes();\n"
        )
        rows = self._palite_mutate(table_id, sql)
        affected = int(rows[0])
        if affected != len(deleted_lsns):
            raise AssertionError(
                f"truncate mismatch: expected to delete {len(deleted_lsns)} "
                f"rows, sqlite reported {affected}")
        return table_id, page_id, kept_lsn, deleted_lsns

    @staticmethod
    def _require_one_change(rows, table_id, page_id, lsn):
        affected = int(rows[0])
        if affected != 1:
            raise AssertionError(
                f"expected 1 affected row, got {affected} for "
                f"table_id={table_id}, page_id={page_id}, lsn={lsn}")

# Shared helpers for tests that exercise WT_SESSION::publish and schema epochs on
# layered tables. Tests using this mixin must define conn_config_follower.
class DisaggSchemaEpochMixin:
    def set_stable_epoch(self, epoch, conn=None):
        """Set stable_disaggregated_schema_epoch on the given (or main) connection."""
        if conn is None:
            conn = self.conn
        conn.set_timestamp(
            'stable_disaggregated_schema_epoch=' + self.timestamp_str(epoch))

    def leader_checkpoint(self, stable_ts, conn=None, session=None):
        """Set the oldest and stable timestamps, then take a timestamped checkpoint."""
        if conn is None:
            conn = self.conn
        if session is None:
            session = self.session
        conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(stable_ts) +
            ',oldest_timestamp=' + self.timestamp_str(1))
        session.checkpoint()

    def publish(self, uri, epoch, session=None):
        """Publish a schema change for uri at the given epoch."""
        if session is None:
            session = self.session
        session.publish(uri, 'disaggregated=(schema_epoch=' + self.timestamp_str(epoch) + ')')

    def stable_uri(self, uri):
        """Return the stable component URI for a given layered table URI."""
        tablename = uri[len('layered:'):]
        return 'file:' + tablename + '.wt_stable'

    def uri_in_shared_metadata(self, conn, uri):
        """Return True if uri's stable constituent is present in the shared metadata table."""
        session = conn.open_session('')
        cursor = session.open_cursor('file:WiredTigerShared.wt_stable', None, None)
        cursor.set_key(self.stable_uri(uri))
        found = cursor.search() == 0
        cursor.close()
        session.close()
        return found

    def uri_in_local_metadata(self, conn, uri):
        """Return True if uri's stable constituent is present in conn's local metadata."""
        session = conn.open_session('')
        exists = True
        try:
            c = session.open_cursor(self.stable_uri(uri))
            c.close()
        except wiredtiger.WiredTigerError:
            exists = False
        session.close()
        return exists

    def open_follower(self):
        """Open a follower, pick up the latest leader checkpoint, and open a session on it."""
        conn = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,' + self.conn_config_follower)
        self.ignoreStdoutPattern('WT_VERB_RTS|(wiredtiger_open:.*WT_VERB_METADATA)')
        self.disagg_advance_checkpoint(conn)
        session = conn.open_session('')
        return conn, session

class DisaggSizeTestMixin:
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def get_checkpoint_size(self, uri=None):
        mc = self.session.open_cursor('metadata:')
        mc.set_key(uri if uri is not None else self.stable_uri)
        self.assertEqual(mc.search(), 0)
        sizes = re.findall(r',size=(\d+),', mc.get_value())
        mc.close()
        self.assertGreater(len(sizes), 0, 'No size= found in checkpoint metadata')
        return int(sizes[-1])

    def get_stat(self, stat_key, uri=None):
        s = self.session.open_cursor('statistics:' + (uri if uri is not None else self.stable_uri))
        val = s[stat_key][2]
        s.close()
        return val

    def get_conn_stat(self, stat_key):
        s = self.session.open_cursor('statistics:')
        val = s[stat_key][2]
        s.close()
        return val
