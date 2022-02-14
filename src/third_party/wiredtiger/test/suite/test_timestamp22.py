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
# test_timestamp22.py
# Misuse the timestamp API, making sure we don't crash.
import wttest, re, suite_random
from wtdataset import SimpleDataSet
from contextlib import contextmanager
from wtscenario import make_scenarios

class test_timestamp22(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=50MB'

    # Keep the number of rows low, as each additional row does
    # not test any new code paths.
    nrows = 3
    uri = "table:test_timestamp22"
    rand = suite_random.suite_random()
    oldest_ts = 0
    stable_ts = 0
    last_durable = 0
    SUCCESS = 'success'
    FAILURE = 'failure'

    format_values = [
        ('integer-row', dict(key_format='i', value_format='S')),
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    # Control execution of an operation, looking for exceptions and error messages.
    # Usage:
    #  with self.expect(self.FAILURE, 'some operation'):
    #     some_operation()  # In this case, we expect it will fail
    #
    # "expected" argument can be self.SUCCESS, self.FAILURE, True, False, for convenience.
    @contextmanager
    def expect(self, expected, message):
        if expected == True:
            expected = self.SUCCESS
        elif expected == False:
            expected = self.FAILURE

        self.pr('TRYING: ' + message + ', expect ' + expected)
        got = None
        # If there are stray error messages from a previous operation,
        # let's find out now.  It can be confusing if we do something illegal
        # here and we have multiple messages to sort out.
        self.checkStderr()

        # 'yield' runs the subordinate operation, we'll catch any resulting exceptions.
        try:
            if expected == self.FAILURE:
                # Soak up any error messages that happen as a result of the failure.
                with self.expectedStderrPattern(r'^.*$', re_flags=re.MULTILINE):
                    yield
            else:
                yield
            got = self.SUCCESS
        except:
            got = self.FAILURE
            self.cleanStderr()

        message += ' got ' + got

        # If we're about to assert, show some extra info
        if expected != got:
            message += ': ERROR expected ' + expected
            self.checkStderr()
        self.pr(message)
        self.assertEquals(expected, got)

    # Create a predictable value based on the iteration number and timestamp.
    def gen_value(self, iternum, ts):
        if self.value_format == '8t':
            return (iternum * 7 + ts * 13) % 255
        return str(iternum) + '_' + str(ts) + '_' + 'x' * 1000

    # Given a number representing an "approximate timestamp", generate a timestamp
    # that is near that number, either plus or minus.
    def gen_ts(self, approx_ts):
        # a number between -10 and 10:
        n = self.rand.rand32() % 21 - 10
        ts = approx_ts + n
        if ts <= 0:
            ts = 1
        return ts

    # Asks whether we should do an illegal operation now. Return yes 5%.
    def do_illegal(self):
        return self.rand.rand32() % 20 == 0

    def report(self, func, arg = None):
        self.pr('DOING: ' + func + ('' if arg == None else '(' + arg + ')'))

    # Insert a set of rows, each insert in its own transaction, with the
    # given timestamps.
    def updates(self, value, ds, do_prepare, commit_ts, durable_ts, read_ts):

        # Generate a configuration for a timestamp_transaction() call.
        # Returns: 1) whether it expects success, 2) config 3) new running commit timestamp
        def timestamp_txn_config(commit_ts, running_commit_ts):
            ok = True
            config = ''
            this_commit_ts = -1
            if self.do_illegal():
                # setting durable timestamp must be after prepare call
                config += ',durable_timestamp=' + self.timestamp_str(self.gen_ts(commit_ts))
                ok = False

            # We don't do the next part if we set an illegal durable timestamp.  It turns out
            # if we do set the durable timestamp illegally, with a valid commit timestamp,
            # the timestamp_transaction() call will fail, but may set the commit timestamp.
            # It makes testing more complex, so we just don't do it.
            elif self.rand.rand32() % 2 == 0:
                if self.do_illegal():
                    this_commit_ts = self.oldest_ts - 1
                elif self.do_illegal():
                    this_commit_ts = self.stable_ts - 1
                else:
                    # It's possible this will succeed, we'll check below.
                    this_commit_ts = self.gen_ts(commit_ts)

                    # OOD does not work with prepared updates. Hence, the commit ts should always be
                    # greater than the last durable ts.
                    if this_commit_ts <= self.last_durable:
                        this_commit_ts = self.last_durable + 1

                config += ',commit_timestamp=' + self.timestamp_str(this_commit_ts)

            if this_commit_ts >= 0:
                if this_commit_ts < running_commit_ts:
                    ok = False
                if this_commit_ts < self.stable_ts:
                    ok = False
                if this_commit_ts < self.oldest_ts:
                    ok = False
            if not ok:
                this_commit_ts = -1
            if this_commit_ts >= 0:
                running_commit_ts = this_commit_ts
            return (ok, config, running_commit_ts)

        session = self.session
        needs_rollback = False
        prepare_config = None
        commit_config = 'commit_timestamp=' + self.timestamp_str(commit_ts)
        tstxn1_config = ''
        tstxn2_config = ''

        ok_commit = do_prepare or not self.do_illegal()
        ok_prepare = True
        ok_tstxn1 = True
        ok_tstxn2 = True

        # Occasionally put a durable timestamp on a commit without a prepare,
        # that will be an error.
        if do_prepare or not ok_commit:
            commit_config += ',durable_timestamp=' + self.timestamp_str(durable_ts)
        cursor = session.open_cursor(self.uri)
        prepare_ts = self.gen_ts(commit_ts)
        prepare_config = 'prepare_timestamp=' + self.timestamp_str(prepare_ts)
        begin_config = '' if read_ts < 0 else 'read_timestamp=' + self.timestamp_str(read_ts)

        # We might do timestamp_transaction calls either before/after inserting
        # values, or both.
        do_tstxn1 = (self.rand.rand32() % 10 == 0)
        do_tstxn2 = (self.rand.rand32() % 10 == 0)

        # Keep track of the commit timestamp that we'll set through the transaction.
        # If it decreases, it will trigger an error.  At the final commit_transaction
        # operation, we'll use the commit_ts.
        running_commit_ts = -1
        first_commit_ts = -1

        if do_tstxn1:
            (ok_tstxn1, tstxn1_config, running_commit_ts) = \
                timestamp_txn_config(commit_ts, running_commit_ts)
            if first_commit_ts < 0:
                first_commit_ts = running_commit_ts

        if do_tstxn2:
            (ok_tstxn2, tstxn2_config, running_commit_ts) = \
                timestamp_txn_config(commit_ts, running_commit_ts)
            if first_commit_ts < 0:
                first_commit_ts = running_commit_ts

        # If a call to set a timestamp fails, a subsequent prepare may assert in diagnostic mode.
        # We consider that acceptable, but we don't test it as it will crash the test suite.
        if not ok_tstxn1 or not ok_tstxn2:
            do_prepare = False      # AVOID ASSERT
            ok_prepare = False
            ok_commit = False

        if running_commit_ts >= 0 and do_prepare:
            # Cannot set prepare timestamp after commit timestamp is successfully set.
            ok_prepare = False

        if do_prepare:
            if commit_ts < prepare_ts:
                ok_commit = False
            if prepare_ts < self.oldest_ts:
                ok_prepare = False

        # If the final commit is too old, we'll fail.
        if commit_ts < self.oldest_ts or commit_ts < self.stable_ts:
            ok_commit = False

        # ODDITY: We don't have to move the commit_ts ahead, but it has to be
        # at least the value of the first commit timestamp set.
        if commit_ts < first_commit_ts:
            ok_commit = False

        # If a prepare fails, the commit fails as well.
        if not ok_prepare:
            ok_commit = False

        msg = 'inserts with commit config(' + commit_config + ')'

        try:
            for i in range(1, self.nrows + 1):
                needs_rollback = False
                if self.do_illegal():
                    # Illegal outside of transaction
                    self.report('prepare_transaction', prepare_config)
                    with self.expect(False, 'prepare outside of transaction'):
                        session.prepare_transaction(prepare_config)

                with self.expect(True, 'begin_transaction(' + begin_config + ')'):
                    session.begin_transaction()
                    needs_rollback = True

                if do_tstxn1:
                    with self.expect(ok_tstxn1, 'timestamp_transaction(' + tstxn1_config + ')'):
                        session.timestamp_transaction(tstxn1_config)

                self.report('set key/value')
                with self.expect(True, 'cursor insert'):
                    cursor[ds.key(i)] = value

                if do_tstxn2:
                    with self.expect(ok_tstxn2, 'timestamp_transaction(' + tstxn2_config + ')'):
                        session.timestamp_transaction(tstxn2_config)

                if do_prepare:
                    self.report('prepare_transaction', prepare_config)
                    with self.expect(ok_prepare, 'prepare'):
                        session.prepare_transaction(prepare_config)

                # Doing anything else after the prepare, like a timestamp_transaction(), will fail
                # with a WT panic.  Don't do that, or else we can't do anything more in this test.

                # If we did a successful prepare and are set up (by virtue of bad timestamps)
                # to do a bad commit, WT will panic, and the test cannot continue.
                # Only proceed with the commit if we have don't have that particular case.
                if ok_commit or not do_prepare or not ok_prepare:
                    needs_rollback = False
                    self.report('commit_transaction', commit_config)
                    with self.expect(ok_commit, 'commit'):
                        session.commit_transaction(commit_config)
                        self.commit_value = value
                        if do_prepare:
                            self.last_durable = durable_ts
                if needs_rollback:
                    # Rollback this one transaction, and continue the loop
                    self.report('rollback_transaction')
                    needs_rollback = False
                    session.rollback_transaction()
        except Exception as e:
            # We don't expect any exceptions, they should be caught as part of self.expect statements.
            self.pr(msg + 'UNEXPECTED EXCEPTION!')
            self.pr(msg + 'fail: ' + str(e))
            raise e
        cursor.close()

    def make_timestamp_config(self, oldest, stable, durable):
        configs = []
        # Get list of 'oldest_timestamp=value' etc. that have non-negative values.
        for ts_name in ['oldest', 'stable', 'durable']:
            val = eval(ts_name)
            if val >= 0:
                configs.append(ts_name + '_timestamp=' + self.timestamp_str(val))
        return ','.join(configs)

    # Determine whether we expect the set_timestamp to succeed.
    def expected_result_set_timestamp(self, oldest, stable, durable):

        # Update the current expected value.  ts is the timestamp being set.
        # If "ts" is negative, ignore it, it's not being set in this call.
        # It is unexpected if "ts" is before the "before" timestamp.
        # The "before" timestamp could be updated during this call
        # with value "before_arg", if not, use the global value for "before".
        def expected_newer(expected, ts, before_arg, before_global):
            if expected and ts >= 0:
                if before_arg >= 0:
                    if before_arg > ts:
                        expected = self.FAILURE
                else:
                    if before_global > ts:
                        expected = self.FAILURE
            return expected

        expected = self.SUCCESS

        # It is a no-op to provide oldest or stable behind the global values. If provided ahead, we
        # will treat the values as if not provided at all.
        if oldest <= self.oldest_ts:
            oldest = -1
        if stable <= self.stable_ts:
            stable = -1

        if oldest >= 0 and stable < 0:
            expected = expected_newer(expected, self.stable_ts, oldest, self.oldest_ts)
        expected = expected_newer(expected, stable, oldest, self.oldest_ts)
        expected = expected_newer(expected, durable, oldest, self.oldest_ts)
        expected = expected_newer(expected, durable, stable, self.stable_ts)

        return expected

    def set_global_timestamps(self, oldest, stable, durable):
        config = self.make_timestamp_config(oldest, stable, durable)
        expected = self.expected_result_set_timestamp(oldest, stable, durable)

        with self.expect(expected, 'set_timestamp(' + config + ')'):
            self.conn.set_timestamp(config)

        # Predict what we expect to happen to the timestamps.
        if expected == self.SUCCESS:
            # If that passes, then independently, oldest and stable can advance, but if they
            # are less than the current value, that is silently ignored.
            if oldest >= self.oldest_ts:
                self.oldest_ts = oldest
                self.pr('updating oldest: ' + str(oldest))
            if stable >= self.stable_ts:
                self.stable_ts = stable
                self.pr('updating stable: ' + str(stable))

        # Make sure the state of global timestamps is what we think.
        expect_query_oldest = self.timestamp_str(self.oldest_ts)
        expect_query_stable = self.timestamp_str(self.stable_ts)
        query_oldest = self.conn.query_timestamp('get=oldest_timestamp')
        query_stable = self.conn.query_timestamp('get=stable_timestamp')

        self.assertEquals(expect_query_oldest, query_oldest)
        self.assertEquals(expect_query_stable, query_stable)
        self.pr('oldest now: ' + query_oldest)
        self.pr('stable now: ' + query_stable)

        if expected == self.FAILURE:
            self.cleanStderr()

    def test_timestamp_randomizer(self):
        # Local function to generate a random timestamp, or return -1
        def maybe_ts(do_gen, iternum):
            if do_gen:
                return self.gen_ts(iternum)
            else:
                return -1

        if wttest.islongtest():
            iterations = 100000
        else:
            iterations = 1000

        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, create_params)

        self.set_global_timestamps(1, 1, -1)

        # Create tables with no entries
        ds = SimpleDataSet(
            self, self.uri, 0, key_format=self.key_format, value_format=self.value_format)

        # We do a bunch of iterations, doing transactions, prepare, and global timestamp calls
        # with timestamps that are sometimes valid, sometimes not. We use the iteration number
        # as an "approximate timestamp", and generate timestamps for our calls that are near
        # that number (within 10).  Thus, as the test runs, the timestamps generally get larger.
        # We always know the state of global timestamps, so we can predict the success/failure
        # on each call.
        self.commit_value = '<NOT_SET>'
        for iternum in range(1, iterations):
            self.pr('\n===== ITERATION ' + str(iternum) + '/' + str(iterations))
            self.pr('RANDOM: ({0},{1})'.format(self.rand.seedw,self.rand.seedz))
            if self.rand.rand32() % 10 != 0:
                commit_ts = self.gen_ts(iternum)
                durable_ts = self.gen_ts(iternum)
                do_prepare = (self.rand.rand32() % 20 == 0)
                if self.rand.rand32() % 2 == 0:
                    read_ts = self.gen_ts(iternum)
                else:
                    read_ts = -1   # no read_timestamp used in txn

                # OOD does not work with prepared updates. Hence, the commit ts should always be
                # greater than the last durable ts.
                if commit_ts <= self.last_durable:
                    commit_ts = self.last_durable + 1

                if do_prepare:
                    # If we doing a prepare, we must abide by some additional rules.
                    # If we don't we'll immediately panic
                    if commit_ts < self.oldest_ts:
                        commit_ts = self.oldest_ts
                    if durable_ts < commit_ts:
                        durable_ts = commit_ts
                    if durable_ts <= self.stable_ts:
                        durable_ts = self.stable_ts + 1
                value = self.gen_value(iternum, commit_ts)
                self.updates(value, ds, do_prepare, commit_ts, durable_ts, read_ts)

            if self.rand.rand32() % 2 == 0:
                # Set some combination of the global timestamps
                r = self.rand.rand32() % 16
                oldest = maybe_ts((r & 0x1) != 0, iternum)
                stable = maybe_ts((r & 0x2) != 0, iternum)
                commit = maybe_ts((r & 0x4) != 0, iternum)
                durable = maybe_ts((r & 0x8) != 0, iternum)
                self.set_global_timestamps(oldest, stable, durable)

        # Make sure the resulting rows are what we expect.
        cursor = self.session.open_cursor(self.uri)
        expect_key = 1
        expect_value = self.commit_value
        for k,v in cursor:
            self.assertEquals(k, expect_key)
            self.assertEquals(v, expect_value)
            expect_key += 1

        # Although it's theoretically possible to never successfully update a single row,
        # with a large number of iterations that should never happen.  I'd rather catch
        # a test code error where we mistakenly don't update any rows.
        self.assertGreater(expect_key, 1)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
