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

# helper_layered_fast_truncate.py
#   Shared helpers for the layered fast truncate Python tests.

from contextlib import closing
from itertools import chain
from typing import Iterable

import wiredtiger


def concat(*iterables):
    """Concatenate any number of iterables into a single list."""
    return list(chain.from_iterable(iterables))


def range_inclusive(start, stop):
    """Return a range covering [start, stop] inclusive."""
    return range(start, stop + 1)


class LayeredFastTruncateConfigMixin:
    """Shared helpers for the layered fast truncate test suite."""

    def key(self, n):
        """
        Convert an int into a key; override in subclasses that use a different
        key format.
        """
        return n

    def session_create_config(self):
        """
        Return the session.create() config string, and, for layered URIs, the
        disaggregated storage options.
        """
        cfg = 'key_format=i,value_format=S'
        uri = getattr(self, 'uri', '')
        if uri.startswith('table'):
            cfg += ',block_manager=disagg,type=layered'
        return cfg

    def auto_closing_cursor(self, config=None):
        """Return a cursor that auto-closes as it goes out of scope."""
        return closing(self.session.open_cursor(self.uri, None, config))

    def populate(self, keys, value='v'):
        """Insert each key with a placeholder value in a single transaction."""
        with self.auto_closing_cursor() as cursor:
            with self.transaction():
                for key in keys:
                    cursor[self.key(key)] = value

    def setup_leader(self, keys=None, extra_cfg=''):
        """
        Create the table on the leader and optionally populate stable. The
        follower picks up these keys via the initial checkpoint.
        """
        self.session.create(self.uri, self.session_create_config() + extra_cfg)
        if keys is not None:
            self.populate(keys)
        self.session.checkpoint()

    def setup_follower(self, keys=None):
        """Switch to follower role and optionally write keys to ingest."""
        self.reopen_disagg_conn('disaggregated=(role="follower"),')
        if keys is not None:
            self.populate(keys)

    def truncate(self, start_key=None, stop_key=None, commit_timestamp=None):
        """
        Truncate [start_key, stop_key] inclusive on self.uri. Either bound
        may be None for an open-ended side. If commit_timestamp is set,
        the truncate transaction commits at that timestamp.
        """
        start = stop = None
        try:
            if start_key is not None:
                start = self.session.open_cursor(self.uri)
                start.set_key(self.key(start_key))
            if stop_key is not None:
                stop = self.session.open_cursor(self.uri)
                stop.set_key(self.key(stop_key))
            # session.truncate() needs a URI iff both cursors are NULL.
            uri = self.uri if (start is None and stop is None) else None
            with self.transaction(commit_timestamp=commit_timestamp):
                self.session.truncate(uri, start, stop, None)
        finally:
            if start is not None:
                start.close()
            if stop is not None:
                stop.close()

    def visible_keys(self, forward=True):
        """Return all keys visible via a scan (forward or backward)."""
        result = []
        with self.auto_closing_cursor() as cursor:
            step = cursor.next if forward else cursor.prev
            with self.transaction(rollback=True):
                while step() == 0:
                    result.append(cursor.get_key())
        return result

    def key_exists(self, key):
        """Return True if key is visible to a search in its own transaction."""
        with self.auto_closing_cursor() as cursor:
            with self.transaction(rollback=True):
                cursor.set_key(self.key(key))
                return cursor.search() == 0

    def search_near_key(self, key):
        """
        Run search_near. Returns (exact, found_key). exact follows WT
        convention: 0 = exact, 1 = positioned above, -1 = positioned
        below, or WT_NOTFOUND if no visible keys exist (in which case
        found_key is None).
        """
        with self.auto_closing_cursor() as cursor:
            with self.transaction(rollback=True):
                cursor.set_key(self.key(key))
                exact = cursor.search_near()
                if exact == wiredtiger.WT_NOTFOUND:
                    return exact, None
                return exact, cursor.get_key()

    def leader_checkpoint(self, ts=None):
        """Set timestamps and checkpoint on the leader."""
        if ts is not None:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts) +
                                    ',oldest_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

    def step_up(self):
        """Promote self.conn_follow to leader; the original leader steps down."""
        self.ignoreStdoutPattern('Picking up the same checkpoint')
        self.disagg_switch_follower_and_leader(self.conn_follow)

    def open_follower(self, table_config='key_format=i,value_format=S'):
        """
        Open a separate follower connection (distinct from setup_follower
        which reopens the existing connection). Returns (conn, session).
        """
        conn = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() +
            ',create,cache_size=50MB,statistics=(all),disaggregated=(role="follower")')
        session = conn.open_session('')
        session.create(self.uri, table_config)
        self.disagg_advance_checkpoint(conn, self.conn)
        return conn, session

    def search_at(self, session, key, ts):
        """Search for key under a read_timestamp; return (ret, value)."""
        cur = session.open_cursor(self.uri)
        try:
            with self.transaction(session=session, read_timestamp=ts, rollback=True):
                cur.set_key(key)
                ret = cur.search()
                val = cur.get_value() if ret == 0 else None
            return ret, val
        finally:
            cur.close()

    def evict_range(self, session, start, stop, step=1):
        """Evict the page(s) backing keys [start, stop] on the given session."""
        evict_cur = session.open_cursor(self.uri, None, 'debug=(release_evict)')
        try:
            with self.transaction(session=session, read_timestamp=10, rollback=True):
                for i in range(start, stop + 1, step):
                    evict_cur.set_key(i)
                    evict_cur.search()
                    evict_cur.reset()
        finally:
            evict_cur.close()

