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
# [TEST_TAGS]
# recovery:log_files
# [END_TAGS]
#
# test_txn19.py
#   Transactions: test recovery with corrupted log files
#

import fnmatch, os, shutil, time
from wtscenario import make_scenarios
from suite_subprocess import suite_subprocess
import wiredtiger, wttest

# This test uses an artificially small log file limit, and creates
# large records so two fit into a log file. This allows us to test
# both the case when corruption happens at the beginning of a log file
# (an even number of records have been created), and when corruption
# happens in the middle of a log file (with an odd number of records).

def corrupt(fname, truncate, offset, writeit):
    with open(fname, 'r+') as log:
        if offset:
            if offset < 0:  # Negative offset means seek to the end
                log.seek(0, 2)
            else:
                log.seek(offset)
        if truncate:
            log.truncate()
        if writeit:
            log.write(writeit)

def copy_for_crash_restart(olddir, newdir):
    ''' Simulate a crash from olddir and restart in newdir. '''
    # with the connection still open, copy files to new directory
    shutil.rmtree(newdir, ignore_errors=True)
    os.mkdir(newdir)
    for fname in os.listdir(olddir):
        fullname = os.path.join(olddir, fname)
        # Skip lock file on Windows since it is locked
        if os.path.isfile(fullname) and \
            "WiredTiger.lock" not in fullname and \
            "Tmplog" not in fullname and \
            "Preplog" not in fullname:
            shutil.copy(fullname, newdir)

class test_txn19(wttest.WiredTigerTestCase, suite_subprocess):
    base_config = 'log=(archive=false,enabled,file_max=100K),' + \
                  'transaction_sync=(enabled,method=none),cache_size=1GB,' + \
                  'debug_mode=(corruption_abort=false),'
    conn_config = base_config
    corruption_type = [
        ('removal', dict(kind='removal', f=lambda fname:
            os.remove(fname))),
        ('truncate', dict(kind='truncate', f=lambda fname:
            corrupt(fname, True, 0, None))),
        ('truncate-middle', dict(kind='truncate-middle', f=lambda fname:
            corrupt(fname, True, 1024 * 25, None))),
        ('zero-begin', dict(kind='zero', f=lambda fname:
            corrupt(fname, False, 0, '\0' * 4096))),
        ('zero-trunc', dict(kind='zero', f=lambda fname:
            corrupt(fname, True, 0, '\0' * 4096))),
        ('zero-end', dict(kind='zero-end', f=lambda fname:
            corrupt(fname, False, -1, '\0' * 4096))),
        ('garbage-begin', dict(kind='garbage-begin', f=lambda fname:
            corrupt(fname, False, 0, 'Bad!' * 1024))),
        ('garbage-middle', dict(kind='garbage-middle', f=lambda fname:
            corrupt(fname, False, 1024 * 25, 'Bad!' * 1024))),
        ('garbage-end', dict(kind='garbage-end', f=lambda fname:
            corrupt(fname, False, -1, 'Bad!' * 1024))),
    ]
    # The list comprehension below expands each entry in the integer tuple
    # list to a scenario.  For example, (3, 4, 2) expands to:
    # ('corrupt=[3,4],checkpoint=2', dict(corruptpos=3, corruptpos2=4, chkpt=2))
    #
    # Each number corresponds to a log file, so for this example, we have
    # corruption in log file 3 (using the style of corruption from
    # corruption_type), there is a second corruption in log file 4,
    # and there is a checkpoint in log file 2.  A value of 0 means no
    # corruption or checkpoint.
    corruption_pos = [
        ('corrupt=[' + str(x) + ',' + str(y) + '],checkpoint=' + str(z),
           dict(corruptpos=x,corruptpos2=y,chkpt=z)) for (x,y,z) in (
               (0, 0, 0), (0, 0, 2), (6, 0, 0), (6, 0, 3), (3, 0, 0),
               (3, 0, 2), (3, 4, 2), (3, 5, 2), (3, 0, 4))]
    nrecords = [('nrecords=10', dict(nrecords=10)),
                ('nrecords=11', dict(nrecords=11))]

    key_format_values = [
        ('integer-row', dict(key_format='i')),
        ('column', dict(key_format='r')),
    ]

    # This function prunes out unnecessary or problematic test cases
    # from the list of scenarios.
    def includeFunc(name, dictarg):
        kind = dictarg['kind']
        corruptpos = dictarg['corruptpos']
        chkpt = dictarg['chkpt']

        # corruptpos == 0 indicates there is no corruption.
        # (i.e. corrupt log file 0, which doesn't exist)
        # We do want to test the case of no corruption, but we don't
        # need to try it against every type of corruption, only one.
        if corruptpos == 0:
            return kind == 'removal'

        # All the other cases are valid
        return True

    scenarios = make_scenarios(
        key_format_values, corruption_type, corruption_pos, nrecords,
        include=includeFunc, prune=20, prunelong=1000)

    uri = 'table:test_txn19'

    # Return the log file number that contains the given record
    # number.  In this test, two records fit into each log file, and
    # after each even record is written, a new log file is created
    # (having no records initially).  The last log file is this
    # (nrecords/2 + 1), given that we start with log 1.
    def record_to_logfile(self, recordnum):
        return recordnum // 2 + 1

    # Returns the first record number in a log file.
    def logfile_to_record(self, logfile):
        return (logfile - 1) * 2

    # Return true if the log file is corrupted.
    # If not corrupted, the log file will produce no errors,
    # and all the records originally written should be recovered.
    def corrupted(self):
        # Corruptpos == 0 means to do no corruption in any log file
        if self.corruptpos == 0:
            return False

        # Adding zeroes to the end of a log file is indistinguishable
        # from having a log file that is preallocated that has not been
        # totally filled. One might argue that if this does not occur
        # in the final log file, it could/should have been truncated.
        # At any rate, we consider this particular corruption to be benign.
        if self.kind == 'zero-end':
            return False
        return True

    def show_logs(self, homedir, msg):
        loglist = []
        for i in range(0, 10):
            basename = 'WiredTigerLog.000000000' + str(i)
            fullname = homedir + os.sep + basename
            if os.path.isfile(fullname):
                loglist.append(i)
                if os.stat(fullname).st_size == 0:
                    self.tty('LOGS ' + msg + ': ' + str(i) + ' is empty')
        self.tty('LOGS ' + msg + ': ' + str(loglist))

    # Generate a value that is a bit over half the size of the log file.
    def valuegen(self, i):
        return str(i) + 'A' * (1024 * 60)   # ~60K

    # Insert a list of keys
    def inserts(self, keylist):
        c = self.session.open_cursor(self.uri)
        for i in keylist:
            if self.chkpt > 0 and self.logfile_to_record(self.chkpt) == i:
                self.session.checkpoint()
            c[i] = self.valuegen(i)
        c.close()

    def checks(self, expectlist):
        c = self.session.open_cursor(self.uri, None, None)
        gotlist = []
        for key, value in c:
            gotlist.append(key)
            self.assertEqual(self.valuegen(key), value)
        self.assertEqual(expectlist, gotlist)
        c.close()

    def log_number_to_file_name(self, homedir, n):
        self.assertLess(n, 10)  # assuming 1 digit
        return homedir + os.sep + 'WiredTigerLog.000000000' + str(n)

    def corrupt_log(self, homedir):
        if not self.corrupted():
            return
        # Mark this test has having corrupted files
        self.databaseCorrupted()
        self.f(self.log_number_to_file_name(homedir, self.corruptpos))

        # Corrupt a second log file if needed
        if self.corruptpos2 != 0:
            self.f(self.log_number_to_file_name(homedir, self.corruptpos2))

    def corrupt_last_file(self):
        return self.corruptpos == self.record_to_logfile(self.nrecords)

    # Corruption past the last written record in a log file can sometimes
    # be detected. In our test case, the last log file has zero or one large
    # 60K record written into it, but it is presized to 100K.  Corruption
    # at the end of this file creates a hole, and the corruption starts
    # a new log record, where it can be detected as phony.  Similarly,
    # corruption in the "middle" of the last file (actually the 25K point)
    # can be detected if there aren't any of the insert records in the file.
    def corrupt_hole_in_last_file(self):
        return self.corrupt_last_file() and \
            ((self.kind == 'garbage-middle' and self.nrecords % 2 == 0) or \
             self.kind == 'garbage-end')

    # Return true iff the log has been damaged in a way that is not detected
    # as a corruption.  WiredTiger must be lenient about damage in any log
    # file, because a partial log record written just before a crash is in
    # most cases indistinguishable from a corruption.  If the beginning of
    # the file is mangled, that is always an unexpected corruption. Situations
    # where we cannot reliably detect corruption include:
    #  - removal of the last log
    #  - certain corruptions at the beginning of a log record (adding garbage
    #      at the end of a log file can trigger this).
    def log_corrupt_but_valid(self):
        if self.corrupt_last_file() and self.kind == 'removal':
            return True
        if self.kind == 'truncate-middle' or \
           self.kind == 'garbage-middle' or \
           self.kind == 'garbage-end':
            return True
        return False

    # In certain cases, we detect log corruption, but just issue warnings.
    def expect_warning_corruption(self):
        if self.kind == 'garbage-middle' and self.chkpt <= self.corruptpos:
            return True
        if self.corrupt_hole_in_last_file():
            return True
        return False

    # For this test, at least, salvage identifies and fixes all
    # recovery failures.
    def expect_salvage_messages(self):
        return self.expect_recovery_failure()

    def expect_recovery_failure(self):
        return self.corrupted() and \
            self.corruptpos >= self.chkpt and \
            not self.log_corrupt_but_valid()

    def recovered_records(self):
        if not self.corrupted() or self.chkpt > self.corruptpos:
            return self.nrecords
        if self.kind == 'garbage-end':
            # All records in the corrupt file will be found.
            found = self.logfile_to_record(self.corruptpos + 1)
        else:
            found = self.logfile_to_record(self.corruptpos)
        return min(found, self.nrecords)

    def test_corrupt_log(self):
        ''' Corrupt the log and restart with different kinds of recovery '''

        # This test creates some data, then simulates a crash with corruption.
        # Then does a restart with recovery, then starts again with salvage,
        # and finally starts again with recovery (adding new records).

        create_params = 'key_format=i,value_format=S'.format(self.key_format)
        self.session.create(self.uri, create_params)
        self.inserts([x for x in range(0, self.nrecords)])
        newdir = "RESTART"
        copy_for_crash_restart(self.home, newdir)
        self.close_conn()
        #self.show_logs(newdir, 'before corruption')
        self.corrupt_log(newdir)
        #self.show_logs(newdir, 'after corruption')
        salvage_config = self.base_config + ',salvage=true'
        expect_fail = self.expect_recovery_failure()

        if expect_fail:
            errmsg = '/WT_TRY_SALVAGE: database corruption detected/'
            if self.kind == 'removal':
                errmsg = '/No such file or directory/'
            if self.kind == 'truncate':
                errmsg = '/failed to read 128 bytes at offset 0/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.reopen_conn(newdir, self.base_config), errmsg)
        else:
            if self.expect_warning_corruption():
                with self.expectedStdoutPattern('log file .* corrupted'):
                    self.reopen_conn(newdir, self.base_config)
            else:
                self.reopen_conn(newdir, self.base_config)
            self.close_conn()

        found_records = self.recovered_records()
        expect = [x for x in range(0, found_records)]

        # If we are salvaging, expect an informational message
        if self.expect_salvage_messages():
            errpat = '.*'
            # Possible messages:
            #   salvage: log files x-y truncated at beginning
            #   salvage: log file x truncated at beginning
            #   salvage: log file x truncated
            #   salvage: log file x removed
            #
            # The removal case may not give an informational error because
            # the log file is already missing, so salvage itself is not
            # removing or truncating any files. It is simply recovering as
            # much as it can.
            #
            if self.kind == 'removal':
                outpat = '^$'
            else:
                outpat = 'salvage: log file'
        else:
            errpat = '^$'
            outpat = '^$'
        with self.expectedStdoutPattern(outpat):
            with self.expectedStderrPattern(errpat):
                self.conn = self.wiredtiger_open(newdir, salvage_config)
                self.session = self.setUpSessionOpen(self.conn)
                self.checks(expect)

        # Insert a couple more and simulate another crash.
        newdir2 = "RESTART2"
        self.inserts([self.nrecords, self.nrecords + 1])
        expect.extend([self.nrecords, self.nrecords + 1])
        copy_for_crash_restart(newdir, newdir2)
        self.checks(expect)
        self.reopen_conn(newdir, self.conn_config)
        self.checks(expect)
        self.reopen_conn(newdir2, self.conn_config)
        self.checks(expect)

class test_txn19_meta(wttest.WiredTigerTestCase, suite_subprocess):
    base_config = 'log=(archive=false,enabled,file_max=100K),' + \
                  'transaction_sync=(enabled,method=none),cache_size=1GB,' + \
                  'debug_mode=(corruption_abort=false),'
    conn_config = base_config

    # The type of corruption to be applied
    corruption_scenarios = [
        ('removal', dict(kind='removal', f=lambda fname:
            os.remove(fname))),
        ('truncate', dict(kind='truncate', f=lambda fname:
            corrupt(fname, True, 0, None))),
        ('truncate-middle', dict(kind='truncate-middle', f=lambda fname:
            corrupt(fname, True, 1024 * 25, None))),
        ('zero-begin', dict(kind='zero', f=lambda fname:
            corrupt(fname, False, 0, '\0' * 4096))),
        ('zero-trunc', dict(kind='zero', f=lambda fname:
            corrupt(fname, True, 0, '\0' * 4096))),
        ('zero-end', dict(kind='zero-end', f=lambda fname:
            corrupt(fname, False, -1, '\0' * 4096))),
        ('garbage-begin', dict(kind='garbage-begin', f=lambda fname:
            corrupt(fname, False, 0, 'Bad!' * 1024))),
        ('garbage-middle', dict(kind='garbage-middle', f=lambda fname:
            corrupt(fname, False, 1024 * 25, 'Bad!' * 1024))),
        ('garbage-end', dict(kind='garbage-end', f=lambda fname:
            corrupt(fname, False, -1, 'Bad!' * 1024))),
    ]
    # File to be corrupted
    filename_scenarios = [
        ('WiredTiger', dict(filename='WiredTiger')),
        ('WiredTiger.basecfg', dict(filename='WiredTiger.basecfg')),
        ('WiredTiger.turtle', dict(filename='WiredTiger.turtle')),
        ('WiredTiger.wt', dict(filename='WiredTiger.wt')),
        ('WiredTigerHS.wt', dict(filename='WiredTigerHS.wt')),
    ]
    # Configure the database type.
    key_format_values = [
        ('integer-row', dict(key_format='i')),
        ('column', dict(key_format='r')),
    ]

    # In many cases, wiredtiger_open without any salvage options will
    # just work.  We list those cases here.
    openable = [
        "removal:WiredTiger.basecfg",
        "removal:WiredTiger.turtle",
        "truncate:WiredTiger",
        "truncate:WiredTiger.basecfg",
        "truncate-middle:WiredTiger",
        "truncate-middle:WiredTiger.basecfg",
        "truncate-middle:WiredTiger.turtle",
        "truncate-middle:WiredTiger.wt",
        "truncate-middle:WiredTigerHS.wt",
        "zero:WiredTiger",
        "zero:WiredTiger.basecfg",
        "zero-end:WiredTiger",
        "zero-end:WiredTiger.basecfg",
        "zero-end:WiredTiger.turtle",
        "zero-end:WiredTiger.wt",
        "zero-end:WiredTigerHS.wt",
        "garbage-begin:WiredTiger",
        "garbage-middle:WiredTiger",
        "garbage-middle:WiredTiger.basecfg",
        "garbage-middle:WiredTiger.turtle",
        "garbage-middle:WiredTiger.wt",
        "garbage-middle:WiredTigerHS.wt",
        "garbage-end:WiredTiger",
        "garbage-end:WiredTiger.turtle",
        "garbage-end:WiredTiger.wt",
        "garbage-end:WiredTigerHS.wt",
    ]

    # The cases for which salvage will not work, represented in the
    # form (self.kind + ':' + self.filename)
    not_salvageable = [
        "removal:WiredTiger.turtle",
        "removal:WiredTiger.wt",
        "truncate:WiredTiger.wt",
        "truncate:WiredTigerHS.wt",
        "zero:WiredTiger.wt",
        "zero:WiredTigerHS.wt",
        "garbage-begin:WiredTiger.basecfg",
        "garbage-begin:WiredTiger.wt",
        "garbage-begin:WiredTigerHS.wt",
        "garbage-end:WiredTiger.basecfg",
    ]

    scenarios = make_scenarios(key_format_values, corruption_scenarios, filename_scenarios)
    uri = 'table:test_txn19_meta_'
    ntables = 5
    nrecords = 1000                                  # records per table.
    suffixes = [ str(x) for x in range(0, ntables)]  # [ '0', '1', ... ]

    def valuegen(self, i):
        return str(i) + 'A' * 1024

    # Insert a list of keys
    def inserts(self, keylist):
        for suffix in self.suffixes:
            c = self.session.open_cursor(self.uri + suffix)
            for i in keylist:
                c[i] = self.valuegen(i)
            c.close()

    def checks(self, expectlist):
        for suffix in self.suffixes:
            c = self.session.open_cursor(self.uri + suffix, None, None)
            gotlist = []
            for key, value in c:
                gotlist.append(key)
                self.assertEqual(self.valuegen(key), value)
            self.assertEqual(expectlist, gotlist)
            c.close()

    def corrupt_meta(self, homedir):
        # Mark this test has having corrupted files
        self.databaseCorrupted()
        filename = os.path.join(homedir, self.filename)
        self.f(filename)

    def is_openable(self):
        key = self.kind + ':' + self.filename
        return key in self.openable

    def is_salvageable(self):
        key = self.kind + ':' + self.filename
        return key not in self.not_salvageable

    def run_wt_and_check(self, dir, expect_fail):
        if expect_fail:
            errmsg = '/WT_TRY_SALVAGE: database corruption detected/'
            if self.filename == 'WiredTigerHS.wt':
                if self.kind == 'removal':
                    errmsg = '/hs_exists/'
                elif self.kind == 'truncate':
                    errmsg = '/file size=0, alloc size=4096/'
            if self.filename == 'WiredTiger.basecfg':
                if self.kind == 'garbage-begin' or self.kind == 'garbage-end':
                    errmsg = '/Bad!Bad!Bad!/'
            if self.filename == 'WiredTiger.wt':
                if self.kind == 'truncate':
                    errmsg = '/is smaller than allocation size; file size=0, alloc size=4096/'
                if self.kind == 'removal':
                    errmsg = '/No such file or directory/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.reopen_conn(dir, self.conn_config), errmsg)
        else:
            # On non-windows platforms, we capture the renaming of WiredTiger.wt file.
            if os.name != 'nt' and self.filename == 'WiredTiger.turtle' and self.kind == 'removal':
                with self.expectedStderrPattern('File exists'):
                    self.reopen_conn(dir, self.conn_config)
                    self.captureout.checkAdditionalPattern(self,
                        'unexpected file WiredTiger.wt found, renamed to WiredTiger.wt.1')
            elif self.filename == 'WiredTiger' and self.kind == 'truncate':
                with self.expectedStdoutPattern("WiredTiger version file is empty"):
                    self.reopen_conn(dir, self.conn_config)
            else:
                self.reopen_conn(dir, self.conn_config)
            self.close_conn()

    def test_corrupt_meta(self):
        newdir = "RESTART"
        newdir2 = "RESTART2"
        expect = list(range(1, self.nrecords + 1))
        salvage_config = self.base_config + ',salvage=true'

        create_params = 'key_format={},value_format=S'.format(self.key_format)
        for suffix in self.suffixes:
            self.session.create(self.uri + suffix, create_params)
        self.inserts(expect)

        # Simulate a crash by copying the contents of the directory
        # before closing.  After we corrupt the copy, make another
        # copy of the corrupted directory.
        #
        # The first corrupted copy will be used to run:
        #    wiredtiger_open without salvage flag, followed by:
        #    wiredtiger_open with salvage flag.
        # The second directory will be used to run:
        #    wiredtiger_open with salvage flag first.

        copy_for_crash_restart(self.home, newdir)
        self.close_conn()
        self.corrupt_meta(newdir)
        copy_for_crash_restart(newdir, newdir2)

        self.run_wt_and_check(newdir, not self.is_openable())

        for salvagedir in [ newdir, newdir2 ]:
            # Removing the 'WiredTiger.turtle' file has weird behavior:
            #  Immediately doing wiredtiger_open (without salvage) succeeds.
            #  Following that, wiredtiger_open w/ salvage also succeeds.
            #
            #  But, immediately after the corruption, if we run
            #  wiredtiger_open with salvage, it will fail.
            # This anomoly should be fixed or explained.
            if self.kind == 'removal' and self.filename == 'WiredTiger.turtle':
                continue

            if self.is_salvageable():
                self.reopen_conn(salvagedir, salvage_config)
                self.checks(expect)
            else:
                # Certain cases are not currently salvageable, they result in
                # an error during the wiredtiger_open.  But the nature of the
                # messages produced during the error is variable by which case
                # it is, and even variable from system to system.
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.reopen_conn(salvagedir, salvage_config),
                    '/.*/')

if __name__ == '__main__':
    wttest.run()
