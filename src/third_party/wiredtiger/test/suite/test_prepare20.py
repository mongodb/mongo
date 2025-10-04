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


# test_prepare20.py
# Check that we can use an application-level log to replay unstable transactions.

import wttest
from wtscenario import make_scenarios
from helper import simulate_crash_restart

class test_prepare20(wttest.WiredTigerTestCase):
    # Do write the logs immediately, but don't waste time fsyncing them.
    conn_config = 'log=(enabled),transaction_sync=(enabled=true,method=none)'

    format_values = [
        ('integer-row', dict(key_format='i', usestrings=True, value_format='S')),
        ('column', dict(key_format='r', usestrings=False, value_format='S')),
        ('column-fix', dict(key_format='r', usestrings=False, value_format='8t')),
    ]
    ckpt_values = [
        ('none', dict(first_ckpt=None, second_ckpt=None)),
        ('1-21', dict(first_ckpt=21, second_ckpt=None)),
        ('1-23', dict(first_ckpt=23, second_ckpt=None)),
        ('1-26', dict(first_ckpt=26, second_ckpt=None)),
        ('2-21', dict(first_ckpt=None, second_ckpt=21)),
        ('2-23', dict(first_ckpt=None, second_ckpt=23)),
        ('2-26', dict(first_ckpt=None, second_ckpt=26)),
        ('2-31', dict(first_ckpt=None, second_ckpt=31)),
        ('2-33', dict(first_ckpt=None, second_ckpt=33)),
        ('2-36', dict(first_ckpt=None, second_ckpt=36)),
    ]
    commit_values = [
        ('commit', dict(commit_before_crash=True)),
        ('nocommit', dict(commit_before_crash=False)),
    ]
    scenarios = make_scenarios(format_values, ckpt_values, commit_values)

    # Application-level log infrastructure. This is real simpleminded and not
    # expected to do very much other than make sure the basic premise works.

    nullvalue = None

    # Some operation codes for the application log.
    BEGIN=1
    WRITE=2
    PREPARETIME=3
    PREPARE=4
    COMMITTIME=5
    DURABLETIME=6
    COMMIT=7

    lsession = None
    lcursor = None
    lsn = 1
    txnid = 1

    log_replays = 0

    def log_open(self, log_uri):
        self.lsession = self.conn.open_session()
        self.lcursor = self.lsession.open_cursor(log_uri)

    def log_begin(self):
        self.session.begin_transaction()
        self.lsession.begin_transaction()
        self.lcursor[self.lsn] = (self.BEGIN, self.txnid, self.nullvalue, self.nullvalue)
        self.lsn += 1

    def log_write(self, dcursor, k, newv):
        oldv = dcursor[k]
        self.lcursor[self.lsn] = (self.WRITE, k, oldv, newv)
        self.lsn += 1
        dcursor[k] = newv

    def log_prepare(self, prepare_ts):
        self.lcursor[self.lsn] = (self.PREPARETIME, prepare_ts, self.nullvalue, self.nullvalue)
        self.lsn += 1
        self.lsession.commit_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(prepare_ts))
        self.lsession.begin_transaction()
        self.lcursor[self.lsn] = (self.PREPARE, self.txnid, self.nullvalue, self.nullvalue)
        self.lsn += 1
        self.lsession.commit_transaction()

    def log_precommit(self, commit_ts, durable_ts):
        self.lsession.begin_transaction()
        self.lcursor[self.lsn] = (self.COMMITTIME, commit_ts, self.nullvalue, self.nullvalue)
        self.lsn += 1
        self.lcursor[self.lsn] = (self.DURABLETIME, durable_ts, self.nullvalue, self.nullvalue)
        self.lsn += 1
        self.lsession.commit_transaction()

    def log_commit(self, commit_ts, durable_ts):
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts) +
            ',durable_timestamp=' + self.timestamp_str(durable_ts))
        self.lsession.begin_transaction()
        self.lcursor[self.lsn] = (self.COMMIT, self.txnid, self.nullvalue, self.nullvalue)
        self.lsn += 1
        self.lsession.commit_transaction()
        self.txnid += 1

    def log_replay(self, dcursor):
        stabletime = int('0x' + self.conn.query_timestamp('get=stable_timestamp'), 16)
        #self.prout("stable {}".format(stabletime))
        # First pass: find prepared txns. Ignore (thus abort) any that didn't even prepare.
        txns = {}
        self.lcursor.reset()
        for _, op, k, oldv, newv in self.lcursor:
            if op == self.BEGIN:
                # "key" is the txnid
                txns[k] = False
            elif op == self.PREPARE or op == self.COMMIT:
                # "key" is the txnid
                txns[k] = True
        # Second pass: replay values from non-aborted txns.
        self.lcursor.reset()
        writing = False
        committime = None
        durabletime = None
        for _, op, k, oldv, newv in self.lcursor:
            if op == self.BEGIN:
                # "key" is the txnid
                if txns[k]:
                    self.session.begin_transaction('roundup_timestamps=(prepared=true)')
                    writing = True
                    #self.prout("begin {}".format(k))
                else:
                    writing = False
            elif op == self.WRITE:
                if writing and dcursor[k] == oldv:
                    dcursor[k] = newv
                    self.log_replays += 1
                    #self.prout("write {} {}".format(k, str(newv)[0:2]))
            elif op == self.PREPARETIME:
                if writing:
                    preparetime = k
                    #self.prout("preparetime {}".format(k))
            elif op == self.PREPARE:
                if writing:
                    self.session.prepare_transaction(
                        'prepare_timestamp=' + self.timestamp_str(preparetime))
                    #self.prout("prepare {}".format(k))
            elif op == self.COMMITTIME:
                if writing:
                    committime = k
                    #self.prout("committime {}".format(k))
            elif op == self.DURABLETIME:
                if writing:
                    durabletime = k
                    if durabletime <= stabletime:
                        durabletime = stabletime + 1
                    #self.prout("durabletime {} -> {}".format(k, durabletime))
            elif op == self.COMMIT:
                if writing:
                    self.session.commit_transaction(
                        'commit_timestamp=' + self.timestamp_str(committime) +
                        ',durable_timestamp=' + self.timestamp_str(durabletime))
                    #self.prout("commit {}".format(k))
                writing = False
                committime = None
                durabletime = None
            else:
                self.assertTrue(False)
        # If we have a prepared and uncommitted transaction, commit it.
        # (Note that we don't handle the case where we write values and don't finish preparing.)
        if committime is not None and durabletime is not None:
            self.assertTrue(writing)
            self.session.commit_transaction(
                'commit_timestamp=' + self.timestamp_str(committime) +
                ',durable_timestamp=' + self.timestamp_str(durabletime))
            #self.prout("final commit")

    # Now the test.

    def test_prepare20(self):
        data_uri = 'file:prepare20data'
        log_uri = 'file:prepare20log'

        # Create one table for data and another to be an application-level log.
        # The log's format is application-lsn -> operation, key, oldvalue, newvalue

        self.session.create(data_uri, 'log=(enabled=false),key_format={},value_format={}'.format(
            self.key_format, self.value_format))

        self.session.create(log_uri, 'key_format=r,value_format=ii{}{}'.format(
            self.value_format, self.value_format))

        nrows = 1000
        if self.value_format == '8t':
            value_a = 97
            value_b = 98
            value_c = 99
            self.nullvalue = 255
        else:
            value_a = 'aaaaa' * 100
            value_b = 'bbbbb' * 100
            value_c = 'ccccc' * 100
            self.nullvalue = ''

        dcursor = self.session.open_cursor(data_uri)

        # Write some baseline data first and make it stable.
        self.session.begin_transaction()
        for i in range(1, nrows + 1):
            dcursor[i] = value_a
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        self.log_open(log_uri)

        # Write and log a transaction.
        self.log_begin()
        for i in range(1, nrows + 1):
            self.log_write(dcursor, i, value_b)
        self.log_prepare(20)
        self.log_precommit(22, 25)
        self.log_commit(22, 25)

        # Optionally checkpoint here.
        if self.first_ckpt is not None:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.first_ckpt))
            self.session.checkpoint()

        # Write and log another transaction.
        self.log_begin()
        for i in range(1, nrows + 1):
            self.log_write(dcursor, i, value_c)
        self.log_prepare(30)

        # Optionally commit now.
        self.log_precommit(32, 35)
        if self.commit_before_crash:
            self.log_commit(32, 35)

        # Optionally checkpoint here.
        if self.second_ckpt is not None:
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.second_ckpt))
            # Use the log session to checkpoint: checkpointing from the main session
            # will fail if we just prepared without committing.
            self.lsession.checkpoint()

        # Now crash.
        simulate_crash_restart(self, ".", "RESTART")
        dcursor = self.session.open_cursor(data_uri)
        self.log_open(log_uri)

        # Replay the application log.
        self.log_replay(dcursor)

        # Make everything stable.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))
        self.session.checkpoint()

        # We should now see all the data.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(15))
        for i in range(1, nrows + 1):
            self.assertEqual(dcursor[i], value_a)
        self.session.rollback_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(25))
        for i in range(1, nrows + 1):
            self.assertEqual(dcursor[i], value_b)
        self.session.rollback_transaction()
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(35))
        for i in range(1, nrows + 1):
            self.assertEqual(dcursor[i], value_c)
        self.session.rollback_transaction()

        # Assert we replayed the right number of writes from the application log.
        ckpt = self.first_ckpt if self.second_ckpt is None else self.second_ckpt
        if ckpt is None or ckpt < 25:
            self.assertEqual(self.log_replays, nrows * 2)
        elif ckpt < 35 or not self.commit_before_crash:
            self.assertEqual(self.log_replays, nrows)
        else:
            self.assertEqual(self.log_replays, 0)
