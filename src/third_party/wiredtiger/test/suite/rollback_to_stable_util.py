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

import os, pathlib, subprocess, wiredtiger, wttest
from time import sleep
from wiredtiger import wiredtiger_strerror, WiredTigerError, WT_ROLLBACK
from wtscenario import make_scenarios

def get_git_root():
    output = subprocess.run(["git", "rev-parse", "--show-toplevel"], check=True, capture_output=True)
    return output.stdout.strip().decode("utf-8")

def get_rts_verify_path():
    rel_path = os.path.join('tools', 'rts_verifier', 'rts_verify.py')
    try:
        root = get_git_root()
        return os.path.join(root, rel_path)
    except:
        pass

    this_dir = pathlib.Path(__file__).parent.resolve()
    return os.path.join(this_dir, '..', '..', rel_path)

def verify_rts_logs():
    binary_path = get_rts_verify_path()
    stdout_path = os.path.join(os.getcwd(), 'stdout.txt')

    if os.name == 'nt':
        output = subprocess.run(['python.exe', binary_path, stdout_path])
    else:
        output = subprocess.run([binary_path, stdout_path])

    stderr = b'' if output.stderr is None else output.stderr
    return (output.returncode, stderr.strip().decode('utf-8'))

# test_rollback_to_stable_base.py
# Shared base class used by rollback to stable tests.
#
# Note: this class now expects self.value_format to have been set for some of the
# operations (those that need to specialize themselves for FLCS).
class test_rollback_to_stable_base(wttest.WiredTigerTestCase):
    # Don't raise errors for these, the expectation is that the RTS verifier will
    # run on the test output.
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')
        self.addTearDownAction(verify_rts_logs)

    def retry_rollback(self, name, txn_session, code):
        retry_limit = 100
        retries = 0
        completed = False
        saved_exception = None
        while not completed and retries < retry_limit:
            if retries != 0:
                self.pr("Retrying operation for " + name)
                if txn_session:
                    txn_session.rollback_transaction()
                sleep(0.1)
                if txn_session:
                    txn_session.begin_transaction()
                    self.pr("Began new transaction for " + name)
            try:
                code()
                completed = True
            except WiredTigerError as e:
                rollback_str = wiredtiger_strerror(WT_ROLLBACK)
                if rollback_str not in str(e):
                    raise(e)
                retries += 1
                saved_exception = e
        if not completed and saved_exception:
            raise(saved_exception)

    def large_updates(self, uri, value, ds, nrows, prepare, commit_ts):
        # Update a large number of records.
        session = self.session
        try:
            cursor = session.open_cursor(uri)
            for i in range(1, nrows + 1):
                if commit_ts == 0:
                    session.begin_transaction('no_timestamp=true')
                else:
                    session.begin_transaction()
                cursor[ds.key(i)] = value
                if commit_ts == 0:
                    session.commit_transaction()
                elif prepare:
                    session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(commit_ts-1))
                    session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
                    session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(commit_ts+1))
                    session.commit_transaction()
                else:
                    session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            cursor.close()
        except WiredTigerError as e:
            rollback_str = wiredtiger_strerror(WT_ROLLBACK)
            if rollback_str in str(e):
                session.rollback_transaction()
            raise(e)

    def large_modifies(self, uri, value, ds, location, nbytes, nrows, prepare, commit_ts):
        # Load a slight modification.
        session = self.session
        try:
            cursor = session.open_cursor(uri)
            session.begin_transaction()
            for i in range(1, nrows + 1):
                cursor.set_key(i)
                # FLCS doesn't support modify (for obvious reasons) so just update.
                # Use the first character of the passed-in value.
                if self.value_format == '8t':
                    cursor.set_value(bytes(value, encoding='utf-8')[0])
                    self.assertEqual(cursor.update(), 0)
                else:
                    mods = [wiredtiger.Modify(value, location, nbytes)]
                    self.assertEqual(cursor.modify(mods), 0)

            if commit_ts == 0:
                session.commit_transaction()
            elif prepare:
                session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(commit_ts-1))
                session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
                session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(commit_ts+1))
                session.commit_transaction()
            else:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            cursor.close()
        except WiredTigerError as e:
            rollback_str = wiredtiger_strerror(WT_ROLLBACK)
            if rollback_str in str(e):
                session.rollback_transaction()
            raise(e)

    def large_removes(self, uri, ds, nrows, prepare, commit_ts):
        # Remove a large number of records.
        session = self.session
        try:
            cursor = session.open_cursor(uri)
            for i in range(1, nrows + 1):
                session.begin_transaction()
                cursor.set_key(i)
                cursor.remove()
                if commit_ts == 0:
                    session.commit_transaction()
                elif prepare:
                    session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(commit_ts-1))
                    session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
                    session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(commit_ts+1))
                    session.commit_transaction()
                else:
                    session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            cursor.close()
        except WiredTigerError as e:
            rollback_str = wiredtiger_strerror(WT_ROLLBACK)
            if rollback_str in str(e):
                session.rollback_transaction()
            raise(e)

    def check(self, check_value, uri, nrows, flcs_extrarows, read_ts):
        # In FLCS, deleted values read back as 0, and (at least for now) uncommitted appends
        # cause zeros to appear under them. If flcs_extrarows isn't None, expect that many
        # rows of zeros following the regular data.
        flcs_tolerance = self.value_format == '8t' and flcs_extrarows is not None

        session = self.session
        if read_ts == 0:
            session.begin_transaction()
        else:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            if flcs_tolerance and count >= nrows:
                self.assertEqual(v, 0)
            else:
                self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows + flcs_extrarows if flcs_tolerance else nrows)
        cursor.close()

    def evict_cursor(self, uri, nrows, check_value):
        # Configure debug behavior on a cursor to evict the page positioned on when the reset API is used.
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction("ignore_prepare=true")
        for i in range (1, nrows + 1):
            evict_cursor.set_key(i)
            self.assertEqual(evict_cursor[i], check_value)
            if i % 10 == 0:
                evict_cursor.reset()
        evict_cursor.close()
        self.session.rollback_transaction()
