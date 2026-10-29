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

import os, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

# Test that blocks written with a larger block header stay readable by a node that knows nothing
# about the added fields, and that a node refuses blocks it is told it cannot interpret.
@disagg_test_class
class test_layered_config15(wttest.WiredTigerTestCase, suite_subprocess):
    test_name = __qualname__
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,'

    # Compression decides how much of the image is copied verbatim, and encryption derives its
    # skip from the header size, so both have to see the larger header.
    encrypt = [
        ('none', dict(encryptor='none', encrypt_args='')),
        ('rotn', dict(encryptor='rotn', encrypt_args='keyid=13')),
    ]
    compress = [
        ('none', dict(block_compress='none')),
        ('snappy', dict(block_compress='snappy')),
    ]

    num_items = 2000
    num_modify = 100
    uri = f"table:{test_name}"

    disagg_storages = gen_disagg_storages(disagg_only = True)
    scenarios = make_scenarios(encrypt, compress, disagg_storages)

    def setUp(self):
        # The block header upgrade debug mode is only available in diagnostic builds.
        if not wiredtiger.diagnostic_build():
            self.skipTest('requires a diagnostic build')
        super().setUp()

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="follower"),' + \
            'encryption=(name={0},{1})'.format(self.encryptor, self.encrypt_args)

    def conn_extensions(self, extlist):
        extlist.extension('compressors', self.block_compress)
        extlist.extension('encryptors', self.encryptor)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def session_create_config(self):
        return 'key_format=S,value_format=S,type=layered,block_compressor={}'.format(
            self.block_compress)

    def value(self, i, modified=False):
        prefix = 'value_mod' if modified else 'value'
        return f'{prefix}{i:04}' + 'abcd' * 100

    def populate(self, count, timestamp, modified=False, start=0):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(start, count):
            cursor[f'key{i:04}'] = self.value(i, modified)
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(timestamp))
        self.session.checkpoint()

    def check_all(self, num_modified):
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.num_items):
            self.assertEqual(cursor[f'key{i:04}'], self.value(i, i < num_modified))
        cursor.close()

    def debug_config(self, mode, v1_ignore_size=False):
        return self.conn_config() + f',debug_mode=(disagg_block_header_upgrade={mode},' + \
            f'disagg_block_header_v1_ignore_size={str(v1_ignore_size).lower()})'

    def test_larger_block_header_is_readable(self):
        """
        A node writing a larger block header records the size in the header itself. A node that
        knows only the smaller header must skip the fields it does not recognise and find the data
        where the writer left it.
        """
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.session.create(self.uri, self.session_create_config())
        self.populate(self.num_items, 1)

        # Restart as a node that writes the larger header, and rewrite part of the table so the
        # larger header reaches storage.
        self.restart_without_local_files(config=self.debug_config('compatible'))
        self.check_all(0)
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.populate(self.num_modify, 2, modified=True)

        # Restart as a node that knows nothing about the added fields. It must read every row,
        # including those in the blocks carrying the larger header.
        self.restart_without_local_files(config=self.debug_config('none'))
        self.check_all(self.num_modify)

        # It must also be able to keep writing on top of them.
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.populate(self.num_modify * 2, 3, modified=True, start=self.num_modify)
        self.check_all(self.num_modify * 2)

        # And a node that writes the larger header again reads what the older node wrote.
        self.restart_without_local_files(config=self.debug_config('compatible'))
        self.check_all(self.num_modify * 2)

    v1_size_warning = 'version 1 block header has combined header size'

    def test_v1_block_header_ignore_size(self):
        """
        With version 1 header sizes ignored, reading a version 1 block whose header is not the
        version 1 size warns, while correctly sized version 1 blocks and larger newer-version blocks
        do not. The option is reconfigurable, so it is switched on at runtime.
        """
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.session.create(self.uri, self.session_create_config())
        self.populate(self.num_items, 1)

        # Correctly sized version 1 blocks do not warn.
        self.restart_without_local_files(config=self.debug_config('none'))
        self.conn.reconfigure('debug_mode=(disagg_block_header_v1_ignore_size=true)')
        self.check_all(0)

        # Larger headers written under a newer version do not warn.
        self.restart_without_local_files(config=self.debug_config('compatible'))
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.populate(self.num_modify, 2, modified=True)
        self.restart_without_local_files(config=self.debug_config('none'))
        self.conn.reconfigure('debug_mode=(disagg_block_header_v1_ignore_size=true)')
        self.check_all(self.num_modify)

        # Version 1 blocks that record a wrong header size warn, and are read using the version 1
        # size. Such blocks are unreadable without the option, and the writer reads them back too,
        # so every node has the option enabled from the start.
        with self.expectedStdoutPattern(self.v1_size_warning):
            self.restart_without_local_files(
                config=self.debug_config('v1_oversized', v1_ignore_size=True))
            self.conn.reconfigure('disaggregated=(role="leader")')
            self.populate(self.num_modify * 2, 3, modified=True, start=self.num_modify)
            self.restart_without_local_files(config=self.debug_config('none', v1_ignore_size=True))
            self.check_all(self.num_modify * 2)

        # The blocks are read again when the test closes the connection.
        self.ignoreStdoutPattern(self.v1_size_warning)

    def subprocess_incompatible_block_header_refused(self):
        """Subprocess body: reading a block that demands a newer reader fails."""
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.session.create(self.uri, self.session_create_config())
        self.populate(self.num_items, 1)

        # Write blocks that declare they need a reader newer than any build here.
        self.restart_without_local_files(config=self.debug_config('incompatible'))
        self.conn.reconfigure('disaggregated=(role="leader")')
        self.populate(self.num_modify, 2, modified=True)

        # Read them back without the debug mode: the blocks demand a newer reader.
        self.restart_without_local_files(config=self.debug_config('none'))
        self.check_all(self.num_modify)

    def test_incompatible_block_header_refused(self):
        """
        A block whose compatible version exceeds the reader's version is refused as corrupt, which
        is fatal, so this runs in a subprocess. The reason is only reported at verbose level, so
        this asserts the failure rather than the message; test_larger_block_header_is_readable is
        the same sequence with a compatible version, and expects success.
        """
        # Set timestamps so the fixture can close the parent connection cleanly.
        self.conn.set_timestamp(
            'stable_timestamp=' + self.timestamp_str(1) +
            ',oldest_timestamp=' + self.timestamp_str(1))

        name = 'incompatible_block_header_refused'
        # Restrict the child to this scenario so each is exercised and asserted independently.
        [returncode, home] = self.run_subprocess_function(f'SUBPROCESS_{name}',
            f'{self.test_name}.{self.test_name}.subprocess_{name}', silent=True,
            scenario=getattr(self, 'scenario_number', None))
        self.assertNotEqual(returncode, 0)
        self.check_file_contains(os.path.join(home, 'stderr.txt'), 'unable to read root page')
