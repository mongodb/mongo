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

import os, shutil
import wiredtiger, wttest
from wiredtiger import stat

# test_config04.py
#    Individually test config options
class test_config04(wttest.WiredTigerTestCase):
    table_name1 = 'test_config04'
    log1 = 'WiredTigerLog.0000000001'
    nentries = 100

    K = 1024
    M = K * K
    G = K * M
    T = K * G

    # Each test needs to set up its connection in its own way,
    # so override these methods to do nothing
    def setUpConnectionOpen(self, dir):
        return None

    def setUpSessionOpen(self, conn):
        return None

    def populate_and_check(self):
        """
        Create entries, and read back in a cursor: key=string, value=string
        """
        create_args = 'key_format=S,value_format=S'
        self.session.create("table:" + self.table_name1, create_args)
        cursor = self.session.open_cursor(
            'table:' + self.table_name1, None, None)
        for i in range(0, self.nentries):
            cursor[str(1000000 + i)] = 'value' + str(i)
        i = 0
        cursor.reset()
        for key, value in cursor:
            self.assertEqual(key, str(1000000 + i))
            self.assertEqual(value, ('value' + str(i)))
            i += 1
        self.assertEqual(i, self.nentries)
        cursor.close()

    def common_test(self, configextra):
        """
        Call wiredtiger_open and run a simple test.
        configextra are any extra configuration strings needed on the open.
        """
        configarg = 'create,statistics=(fast)'
        if configextra != None:
            configarg += ',' + configextra
        self.conn = self.wiredtiger_open('.', configarg)
        self.session = self.conn.open_session(None)
        self.populate_and_check()

    def common_cache_size_test(self, sizestr, size):
        self.common_test('cache_size=' + sizestr)
        cursor = self.session.open_cursor('statistics:', None, None)
        self.assertEqual(cursor[stat.conn.cache_bytes_max][2], size)
        cursor.close()

    def common_log_test(self, path, dirname):
        self.common_test('log=(enabled,' + path + ',remove=false)')
        self.assertTrue(os.path.exists(dirname + os.sep + self.log1))

    def test_bad_config(self):
        msg = '/unknown configuration key/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', 'not_valid,another_bad=10'),
            msg)

    def test_cache_size_number(self):
        # Use a number without multipliers
        # 1M is the minimum, we'll ask for 1025 * 1024
        cache_size_str = str(1025 * 1024)
        self.common_cache_size_test(cache_size_str, 1025*self.K)

    def test_cache_size_K(self):
        # Kilobyte sizing test
        # 1M is the minimum, so ask for that using K notation.
        self.common_cache_size_test('1024K', 1024*self.K)

    def test_cache_size_M(self):
        # Megabyte sizing test
        self.common_cache_size_test('30M', 30*self.M)

    def test_cache_size_G(self):
        # Gigabyte sizing test
        # We are specifying the maximum the cache can grow,
        # not the initial cache amount, so small tests like
        # this can still run on smaller machines.
        self.common_cache_size_test('7G', 7*self.G)

    def test_cache_size_T(self):
        # Terabyte sizing test
        # We are specifying the maximum the cache can grow,
        # not the initial cache amount, so small tests like
        # this can still run on smaller machines.
        self.common_cache_size_test('2T', 2*self.T)

    def test_cache_too_small(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', 'create,cache_size=900000'),
            "/Value too small for key 'cache_size' the minimum is/")

    def test_cache_too_large(self):
        T11 = 11 * self.T  # 11 Terabytes
        configstr = 'create,cache_size=' + str(T11)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', configstr),
            "/Value too large for key 'cache_size' the maximum is/")

    def test_eviction(self):
        self.common_test('eviction_target=84,eviction_trigger=94')

    def test_eviction_bad(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.', 'create,eviction_target=91,' +
                                 'eviction_trigger=81'),
            "/eviction target must be lower than the eviction trigger/")

    def test_eviction_bad2(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.', 'create,eviction_target=86,' +
                                 'eviction_trigger=86'),
            "/eviction target must be lower than the eviction trigger/")

    def test_eviction_absolute(self):
        self.common_test('eviction_target=50MB,eviction_trigger=60MB,'
            'eviction_dirty_target=20MB,eviction_dirty_trigger=25MB,'
            'eviction_checkpoint_target=13MB')

    def test_eviction_abs_and_pct(self):
        self.common_test('eviction_target=50,eviction_trigger=60MB,'
             'eviction_dirty_target=20,eviction_dirty_trigger=25MB')

    def test_eviction_abs_less_than_one_pct(self):
        self.wiredtiger_open('.','create,cache_size=8GB,eviction_target=70MB,'
                             'eviction_trigger=75MB')

    # Test that eviction_target must be lower than eviction_trigger
    def test_eviction_absolute_bad(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,eviction_target=70MB,'
                                 'eviction_trigger=60MB'),
            '/eviction target must be lower than the eviction trigger/')

    def test_eviction_abs_and_pct_bad(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
             self.wiredtiger_open('.','create,eviction_target=50,'
                                  'eviction_trigger=40MB'),
             '/eviction target must be lower than the eviction trigger/')

    def test_eviction_abs_and_pct_bad2(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
             self.wiredtiger_open('.','create,eviction_target=50MB,'
                                  'eviction_trigger=40'),
             '/eviction target must be lower than the eviction trigger/')

    def test_eviction_tgt_abs_too_large(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,cache_size=500MB,'
                                 'eviction_target=1G'),
            '/eviction target should not exceed cache size/')

    def test_eviction_trigger_abs_too_large(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,cache_size=500MB,'
                                 'eviction_trigger=1G'),
            '/eviction trigger should not exceed cache size/')

    def test_eviction_dirty_tgt_abs_too_large(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,cache_size=500MB,'
                                 'eviction_dirty_target=1G'),
            '/eviction dirty target should not exceed cache size/')

    def test_eviction_dirty_trigger_abs_too_large(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,cache_size=500MB,'
                                 'eviction_dirty_trigger=1G'),
            '/eviction dirty trigger should not exceed cache size/')

    def test_eviction_dirty_trigger_abs_equal_to_dirty_target(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,eviction_dirty_trigger=10MB,'
                                 'eviction_dirty_target=10MB'),
            '/eviction dirty target must be lower than the eviction dirty trigger/')

    def test_eviction_dirty_trigger_abs_too_low(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,eviction_dirty_trigger=9MB,'
                                 'eviction_dirty_target=10MB'),
            '/eviction dirty target must be lower than the eviction dirty trigger/')

    def test_eviction_checkpoint_tgt_abs_too_large(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,cache_size=500MB,'
                                 'eviction_checkpoint_target=1G'),
            '/eviction checkpoint target should not exceed cache size/')

    def test_eviction_updates_tgt_abs_too_large(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,cache_size=500MB,'
                                 'eviction_updates_target=1G'),
            '/eviction updates target should not exceed cache size/')

    def test_eviction_updates_trigger_abs_equal_to_updates_target(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,eviction_updates_target=10MB,'
                                 'eviction_updates_trigger=10MB'),
            '/eviction updates target must be lower than the eviction updates trigger/')

    def test_eviction_updates_trigger_abs_too_low(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.wiredtiger_open('.','create,eviction_updates_trigger=9MB,'
                                 'eviction_updates_target=10MB'),
            '/eviction updates target must be lower than the eviction updates trigger/')

    def test_invalid_config(self):
        msg = '/Unbalanced brackets/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', '}'), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', '{'), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', '{}}'), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', '(]}'), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', '(create=]}'), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', '(create='), msg)

    def test_error_prefix(self):
        self.common_test('error_prefix="MyOwnPrefix"')
        # TODO: how do we verify that it was set?

    def test_logging(self):
        # Test variations on the log configuration.  The log test takes
        # a configuration string as the first arg and the directory pathname
        # to confirm the existence of the log file.  For now we're testing
        # the log pathname only.
        #
        # Test the default in the home directory.
        self.common_log_test('', '.')
        self.conn.close()

        # Test a subdir of the home directory.
        logdirname = 'logdir'
        logdir = '.' + os.sep + logdirname
        os.mkdir(logdir)
        confstr = 'path=' + logdirname
        self.common_log_test(confstr, logdir)
        self.conn.close()

        # Test an absolute path directory.
        if os.name == 'posix':
            logdir = os.path.abspath('absolutelogdir')
            os.mkdir(logdir)
            confstr = 'path=' + logdir
            self.common_log_test(confstr, logdir)
            self.conn.close()
            shutil.rmtree(logdir, ignore_errors=True)

    def test_multiprocess(self):
        self.common_test('multiprocess')
        # TODO: how do we verify that it was set?

    def test_session_max(self):
        # Note: There isn't any direct way to know that this was set,
        # but we'll have a separate functionality test to test for
        # this indirectly.
        self.common_test('session_max=99')

    def test_transactional(self):
        # Note: this will have functional tests in the future.
        self.common_test('')

if __name__ == '__main__':
    wttest.run()
