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

import os
import wiredtiger
import wttest

# Test that deprecated block cache configuration is handled correctly.
class test_block_cache_deprecate(wttest.WiredTigerTestCase):
    msg = '/block cache has been deprecated and is no longer supported/'

    # Test that opening with the block cache enabled fails with an appropriate error.
    def test_block_cache_enabled_error(self):
        self.close_conn()

        config = 'create,block_cache=(enabled=true)'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: self.wiredtiger_open('.', config), self.msg)

        # Reopen normally so tearDown can close cleanly.
        self.open_conn()

    # Test that opening with the block cache disabled issues a deprecation warning.
    def test_block_cache_disabled_warning(self):
        self.close_conn()

        config = 'create,block_cache=(enabled=false)'
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            self.open_conn('.', config)

    # Test that a disabled NVRAM block cache touches no paths, and that configuration
    # validation is unchanged.
    def test_block_cache_disabled_nvram(self):
        self.close_conn()

        config = 'create,block_cache=(enabled=false,type=nvram,nvram_path=/nonexistent)'
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            self.open_conn('.', config)

        # Unknown block cache sub-keys are still a configuration error.
        self.close_conn()
        config = 'create,block_cache=(foo=bar)'
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: self.wiredtiger_open('.', config),
                '/unknown configuration key/')

        # Reopen normally so tearDown can close cleanly.
        self.open_conn()

    # Test that block cache configuration is not persisted into a new home's base
    # configuration file.
    def test_block_cache_not_persisted(self):
        self.close_conn()

        os.mkdir('B')
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            conn = self.wiredtiger_open('B', 'create,block_cache=(enabled=false)')
        conn.close()

        self.assertTrue(os.path.exists('B/WiredTiger.basecfg'))
        with open('B/WiredTiger.basecfg') as basecfg_file:
            self.assertNotIn('block_cache', basecfg_file.read())

    # Test that a persisted enabled block cache in the base configuration file is
    # ignored: the base configuration file is machine-written, it must not keep the
    # home from opening.
    def test_block_cache_basecfg(self):
        self.close_conn()

        # Create a database, then persist an enabled block cache in its base
        # configuration file.
        os.mkdir('A')
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            conn = self.wiredtiger_open('A', 'create,block_cache=(enabled=false)')
        conn.close()
        self.assertTrue(os.path.exists('A/WiredTiger.basecfg'))
        with open('A/WiredTiger.basecfg', 'a') as basecfg_file:
            basecfg_file.write('block_cache=(enabled=true)\n')

        # The home opens, the persisted setting is ignored with a warning.
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            conn = self.wiredtiger_open('A', '')
        conn.close()

    # Test that WiredTiger.config is treated as a user-supplied source: enablement there is
    # rejected, disabled settings are ignored.
    def test_block_cache_user_config(self):
        self.close_conn()

        # Create a database.
        os.mkdir('D')
        conn = self.wiredtiger_open('D', 'create')
        conn.close()

        # Enabling in WiredTiger.config is rejected.
        with open('D/WiredTiger.config', 'w') as user_config:
            user_config.write('block_cache=(enabled=true)\n')
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: self.wiredtiger_open('D', ''), self.msg)

        # A disabled setting is ignored with a warning.
        with open('D/WiredTiger.config', 'w') as user_config:
            user_config.write('block_cache=(enabled=false)\n')
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            conn = self.wiredtiger_open('D', '')
        conn.close()

    # Test that the WIREDTIGER_CONFIG environment variable is treated as a user-supplied
    # source: enablement there is rejected, disabled settings are ignored.
    def test_block_cache_env_config(self):
        self.close_conn()

        # Create a database.
        os.mkdir('E')
        conn = self.wiredtiger_open('E', 'create')
        conn.close()

        saved = os.environ.pop('WIREDTIGER_CONFIG', None)
        try:
            # Enabling through the environment is rejected.
            os.environ['WIREDTIGER_CONFIG'] = 'block_cache=(enabled=true)'
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, lambda: self.wiredtiger_open('E', ''), self.msg)

            # A disabled setting is ignored with a warning.
            os.environ['WIREDTIGER_CONFIG'] = 'block_cache=(enabled=false)'
            with self.expectedStdoutPattern(
                'block cache has been deprecated and is no longer supported'):
                conn = self.wiredtiger_open('E', '')
            conn.close()
        finally:
            if saved is None:
                del os.environ['WIREDTIGER_CONFIG']
            else:
                os.environ['WIREDTIGER_CONFIG'] = saved

    # Test that the removed block cache verbose category is rejected as an invalid choice.
    def test_block_cache_verbose(self):
        self.close_conn()

        config = 'create,verbose=[block_cache]'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: self.wiredtiger_open('.', config),
            '/not a permitted choice for key .verbose./')

        # Reopen normally so tearDown can close cleanly.
        self.open_conn()

    # Test that reconfiguring with block cache settings is ignored or rejected.
    def test_block_cache_reconfigure(self):
        # Enabling is rejected.
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure('block_cache=(enabled=true)'), self.msg)

        # A rejected mixed reconfigure leaves the connection usable.
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure('block_cache=(enabled=true),statistics=(all)'), self.msg)
        self.conn.reconfigure('statistics=(all)')

        # The block cache check runs before the compatibility change: a rejected reconfigure
        # mixing both fails with the block cache error, not a compatibility error.
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure(
                'block_cache=(enabled=true),compatibility=(release="99.9")'), self.msg)
        self.conn.reconfigure('statistics=(all)')

        # A disabled block cache is ignored with a warning.
        with self.expectedStdoutPattern(
            'block cache has been deprecated and is no longer supported'):
            self.conn.reconfigure('block_cache=(enabled=false)')
