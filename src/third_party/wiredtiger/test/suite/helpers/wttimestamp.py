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
from contextlib import contextmanager
import wttest, wiredtiger

# Timestamp abstraction class
class WiredTigerTimeStamp(object):
    def __init__(self, initial_timestamp=1):
        self.ts = initial_timestamp

    def incr(self):
        self.ts += 1

    def get_incr(self):
        ret = self.ts
        self.ts += 1
        return ret

    def get(self):
        return self.ts

# This allows us to easily "wrap" an operation in a transaction at the
# next timestamp.  This global function version can be called externally.
@contextmanager
def session_timestamped_transaction(session, timestamper):
    need_commit = False
    if timestamper is not None and \
      not (hasattr(session, "_has_transaction") and session._has_transaction):
        session.begin_transaction()
        need_commit = True
    yield
    if need_commit:
        config = 'commit_timestamp=%x' % timestamper.get_incr()
        #wttest.WiredTigerTestCase.tty('commit_transaction ' + config)
        session.commit_transaction(config)
    elif timestamper is not None:
        config = 'commit_timestamp=%x' % timestamper.get_incr()
        #wttest.WiredTigerTestCase.tty('timestamp_transaction ' + config)
        session.timestamp_transaction(config)

# This class acts as a "proxy" for a Cursor.  All methods, etc.
# are passed to the implementation object (via __getattr__),
# except for the ones that we explicitly override here.
class TimestampedCursor(wiredtiger.Cursor):
    def __init__(self, session, cursor, timeStamper, testcase):
        self._cursor = cursor
        self._timeStamper = timeStamper
        self._testcase = testcase
        self._session = session
        if not hasattr(session, "_has_transaction"):
            session._has_transaction = False

    def __getattr__(self, name):
        return getattr(self._cursor, name)

    # A more convenient way to "wrap" an operation in a transaction
    @contextmanager
    def timestamped_transaction(self):
        with session_timestamped_transaction(self._session, self._timeStamper):
            yield

    # Overrides Cursor.insert
    def insert(self):
        with self.timestamped_transaction():
            return self._cursor.insert()

    # Overrides Cursor.update
    def update(self):
        with self.timestamped_transaction():
            return self._cursor.update()

    # Overrides Cursor.remove
    def remove(self):
        with self.timestamped_transaction():
            return self._cursor.remove()

    # Overrides Cursor.modify
    def modify(self, mods):
        with self.timestamped_transaction():
            return self._cursor.modify(mods)
