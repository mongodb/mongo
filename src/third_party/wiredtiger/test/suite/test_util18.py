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

import codecs, filecmp
from suite_subprocess import suite_subprocess
import wttest
from wtscenario import make_scenarios

# test_util18.py
#   Utilities: wt printlog
class test_util18(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util18.a'
    uri = 'table:' + tablename
    logmax = 100
    nentries = 5
    create_params = 'key_format=S,value_format=S'
    key_prefix = 'KEY'
    val_prefix = 'VAL'

    # Whether user data is redacted or printed.
    print_user_data = [
        ('show_user_data', dict(print_user_data=True)),
        ('no_user_data', dict(print_user_data=False)),
    ]

    scenarios = make_scenarios(print_user_data)

    def conn_config(self):
        return 'log=(enabled,file_max=%dK,remove=false)' % self.logmax

    # Populate our test table with data we can check against in the printlog output.
    def populate(self):
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(0, self.nentries):
            key = self.key_prefix + str(i)
            val = self.val_prefix + str(i)
            cursor[key] = val
        cursor.close()

    # Check the given printlog file reflects the data written by 'populate'.
    def check_populated_printlog(self, log_file, expect_keyval, expect_keyval_hex):
        for i in range(0, self.nentries):
            key = self.key_prefix + str(i)
            val = self.val_prefix + str(i)
            # Check if the KEY/VAL commits exist in the log file.
            if expect_keyval:
                self.check_file_contains(log_file, '"key": "%s\\u0000"' % key)
                self.check_file_contains(log_file, '"value": "%s\\u0000"' % val)
            else:
                self.check_file_not_contains(log_file, '"key": "%s\\u0000"' % key)
                self.check_file_not_contains(log_file, '"value": "%s\\u0000"' % val)

            # Convert our KEY/VAL strings to their expected hex value.
            hex_key = codecs.encode(key.encode(), 'hex')
            val_key = codecs.encode(val.encode(), 'hex')
            # Check if the KEY/VAL commits exist in the log file (in hex form).
            if expect_keyval_hex:
                self.check_file_contains(log_file, '"key-hex": "%s00"' % str(hex_key, 'ascii'))
                self.check_file_contains(log_file, '"value-hex": "%s00"' % str(val_key, 'ascii'))
            else:
                self.check_file_not_contains(log_file, '"key-hex": "%s00"' % str(hex_key, 'ascii'))
                self.check_file_not_contains(log_file, '"value-hex": "%s00"' % str(val_key, 'ascii'))

    def test_printlog_file(self):
        """
        Run printlog on a populated table.
        """
        self.session.create('table:' + self.tablename, self.create_params)
        self.populate()
        wt_args = ["printlog"]
        # Append "-u" if we expect printlog to print user data.
        if self.print_user_data:
            wt_args.append("-u")
        self.runWt(wt_args, outfilename='printlog.out')
        self.check_non_empty_file('printlog.out')
        self.check_populated_printlog('printlog.out', self.print_user_data, False)

    def test_printlog_hex_file(self):
        """
        Run printlog with hexadecimal formatting on a populated table.
        """
        self.session.create('table:' + self.tablename, self.create_params)
        self.populate()
        wt_args = ["printlog", "-x"]
        # Append "-u" if we expect printlog to print user data.
        if self.print_user_data:
            wt_args.append("-u")
        self.runWt(wt_args, outfilename='printlog-hex.out')
        self.check_non_empty_file('printlog-hex.out')
        self.check_populated_printlog('printlog-hex.out', self.print_user_data, self.print_user_data)

    def test_printlog_message(self):
        """
        Run printlog with messages-only formatting on a populated table.
        """
        self.session.create('table:' + self.tablename, self.create_params)
        self.populate()
        # Write a log message that we can specifically test the presence of.
        log_message = "Test Message: %s" % self.tablename
        self.session.log_printf(log_message)
        wt_args = ["printlog", "-m"]
        # Append "-u" if we expect printlog to print user data.
        if self.print_user_data:
            wt_args.append("-u")
        self.runWt(wt_args, outfilename='printlog-message.out')
        self.check_non_empty_file('printlog-message.out')
        self.check_file_contains('printlog-message.out', log_message)
        self.check_populated_printlog('printlog-message.out', False, False)

    def test_printlog_lsn_offset(self):
        """
        Run printlog with an LSN offset provided.
        """
        self.session.create('table:' + self.tablename, self.create_params)
        self.populate()

        # Open a log cursor to accurately extract the first, second and last LSN from our
        # log.
        c = self.session.open_cursor("log:", None, None)
        # Moving the cursor to the beginning of the file, extract our first LSN.
        c.next()
        first_lsn_keys = c.get_key()
        # Moving the cursor, extract our second LSN.
        c.next()
        second_lsn_keys = c.get_key()
        last_lsn_keys = []
        # Moving the cursor to the last available key, extract the last LSN value.
        while c.next() == 0:
            last_lsn_keys = c.get_key()
            c.next()
        c.close()

        # Construct the first, second and last LSN values, assuming the
        # key elements follow the following sequence: [lsn.file, lsn.offset, opcount].
        first_lsn = '%s,%s' % (first_lsn_keys[0], first_lsn_keys[1])
        second_lsn = '%s,%s' % (second_lsn_keys[0], second_lsn_keys[1])
        last_lsn = '%s,%s' % (last_lsn_keys[0], last_lsn_keys[1])

        # Test printlog on a bounded range that starts and ends on our first LSN record. In doing so we want
        # to assert that other log records won't be printed e.g. the second LSN record.
        wt_args = ["printlog", '-l %s,%s' % (first_lsn, first_lsn)]
        self.runWt(wt_args, outfilename='printlog-lsn-offset.out')
        self.check_file_contains('printlog-lsn-offset.out', '"lsn" : [%s]' % first_lsn)
        self.check_file_not_contains('printlog-lsn-offset.out', '"lsn" : [%s]' % second_lsn)
        self.check_populated_printlog('printlog-lsn-offset.out', False, False)

        # Test printlog from the starting LSN value to the end of the log. We expect to find the logs relating
        # to the population of our table.
        wt_args = ["printlog", '-l %s' % first_lsn]
        # Append "-u" if we expect printlog to print user data.
        if self.print_user_data:
            wt_args.append("-u")
        self.runWt(wt_args, outfilename='printlog-lsn-offset.out')
        self.check_populated_printlog('printlog-lsn-offset.out', self.print_user_data, False)

        # Test that using LSN '1,0' and our first LSN value produce the same output when passed to printlog.
        # We expect printing from LSN '1,0' (which should denote to the beginning of the first log file)
        # is equivalent to printing from our first extracted LSN value to the last LSN value.
        wt_args_beginning = ["printlog", '-l 1,0,%s' % last_lsn]
        wt_args_first = ["printlog", '-l %s,%s' % (first_lsn, last_lsn)]
        if self.print_user_data:
            wt_args_beginning.append("-u")
            wt_args_first.append("-u")
        self.runWt(wt_args_beginning, outfilename='printlog-lsn-offset-beginning.out')
        self.runWt(wt_args_first, outfilename='printlog-lsn-offset-first.out')
        self.assertTrue(filecmp.cmp('printlog-lsn-offset-beginning.out', 'printlog-lsn-offset-first.out'))

if __name__ == '__main__':
    wttest.run()
