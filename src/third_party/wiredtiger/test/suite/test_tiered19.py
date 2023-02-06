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

import random, string, wiredtiger, wttest
from helper_tiered import get_auth_token, TieredConfigMixin
from wtscenario import make_scenarios

file_system = wiredtiger.FileSystem

# test_tiered19.py
# Testing storage source functionality for the Azure Storage Store
# and Google Cloud extensions.
class test_tiered19(wttest.WiredTigerTestCase, TieredConfigMixin):

    tiered_storage_sources = [
        ('azure_store', dict(is_tiered = True,
            is_local_storage = False,
            auth_token = get_auth_token('azure_store'), 
            bucket = 'pythontest',
            bucket_prefix = "pfx_",
            ss_name = 'azure_store')),
        ('gcp_store', dict(is_tiered = True,
            is_local_storage = False,
            auth_token = get_auth_token('gcp_store'), 
            bucket = 'test_tiered19',
            bucket_prefix = "pfx_",
            ss_name = 'gcp_store')),
    ]

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(tiered_storage_sources)

    def get_storage_source(self):
        return self.conn.get_storage_source(self.ss_name)

    def get_fs_config(self, prefix = '', cache_dir = ''):
        conf = ''
        if prefix:
            conf += ',prefix=' + prefix
        if cache_dir:
            conf += ',cache_directory=' + cache_dir
        return conf
    
    # Load the storage source extensions.
    def conn_extensions(self, extlist):
        TieredConfigMixin.conn_extensions(self, extlist)

    def test_gcp_filesystem(self): 
        # Test basic functionality of the storage source API, calling
        # each supported method in the API at least once.

        session = self.session
        ss = self.get_storage_source()

        if (self.ss_name != 'gcp_store'):
            return

        # Since this class has multiple tests, append test name to the prefix to
        # avoid namespace collision. 0th element on the stack is the current function.
        prefix = self.bucket_prefix.join(random.choices(string.ascii_letters + string.digits, k=10))

        # Success case: an existing accessible bucket has been provided with the correct credentials file.
        fs = ss.ss_customize_file_system(session, self.bucket, self.auth_token, self.get_fs_config(prefix))

        # Error cases.
        err_msg = 'Exception: Invalid argument'

        # Do not provide bucket name and credentials.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, None, None, self.get_fs_config(prefix)), err_msg)
        # Provide empty bucket string.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, "", None, self.get_fs_config(prefix)), err_msg)
        # Provide credentials in incorrect form.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, self.bucket, "gcp_cred", self.get_fs_config(prefix)), err_msg)
        # Provide empty credentials string.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, self.bucket, "", self.get_fs_config(prefix)), err_msg)
        # Provide a bucket name that does not exist.
        non_exist_bucket = "non_exist" 
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, non_exist_bucket, None, self.get_fs_config(prefix)), err_msg)
        # Provide a bucket name that exists but we do not have access to. 
        no_access_bucket = "test_cred"
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, no_access_bucket, None, self.get_fs_config(prefix)), err_msg)

        fs.terminate(session)
        ss.terminate(session)

    def test_ss_file_systems_gcp_and_azure(self):
        if self.ss_name != "azure_store":
            return
        session = self.session
        ss = self.get_storage_source()

        prefix_1 = self.bucket_prefix.join(
            random.choices(string.ascii_letters + string.digits, k=10))
        prefix_2 = self.bucket_prefix.join(
            random.choices(string.ascii_letters + string.digits, k=10))

        # Test the customize file system function errors when there is an invalid bucket.
        err_msg = '/Exception: Invalid argument/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, "", None, self.get_fs_config(prefix_1)), err_msg)

        bad_bucket = "./bucket_BAD" 
        err_msg = '/Exception: No such file or directory/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, bad_bucket, None, self.get_fs_config(prefix_1)), err_msg)

        # Test the customize file system function works when there is a valid bucket.
        azure_fs_1 = ss.ss_customize_file_system(
            session, self.bucket, None, self.get_fs_config(prefix_1))

        # Create another file systems to make sure that terminate works.
        ss.ss_customize_file_system(
            session, self.bucket, None, self.get_fs_config(prefix_2))
        
        # Test that azure file system terminate succeeds.
        self.assertEqual(azure_fs_1.terminate(session), 0)

        # Test that azure storage source terminate succeeds.
        self.assertEqual(ss.terminate(session), 0)
