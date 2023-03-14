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
            bucket_prefix = 'pfx_',
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

        if self.ss_name != 'gcp_store':
            return

        session = self.session
        ss = self.get_storage_source()

        # Since this class has multiple tests, append test name to the prefix to
        # avoid namespace collision. 0th element on the stack is the current function.
        prefix = self.bucket_prefix.join(random.choices(string.ascii_letters + string.digits, k=10))

        # Success case: an existing accessible bucket has been provided with the correct credentials file.
        fs = ss.ss_customize_file_system(session, self.bucket, self.auth_token, self.get_fs_config(prefix))

        # Error cases.
        err_msg = '/Exception: Invalid argument/'

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
                session, non_exist_bucket, self.auth_token, self.get_fs_config(prefix)), err_msg)
        # Provide a bucket name that exists but we do not have access to.
        no_access_bucket = "test_cred"
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, no_access_bucket, self.auth_token, self.get_fs_config(prefix)), err_msg)

        # Test fs_open_file fails when the target file is not in the bucket and does not exist locally.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: fs.fs_open_file(
                session, 'test_put', file_system.open_file_type_data, file_system.open_readonly), err_msg)

        # We cannot use the file system to create files, it is read-only. So we use python I/O to
        # build up the file.
        f = open('foobar', 'wb')

        # Test fs_open_file fails when the target file exists locally but is not in the bucket.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: fs.fs_open_file(
                session, 'foobar', file_system.open_file_type_data, file_system.open_readonly), err_msg)

        # The file system is read only so cannot be used to create files because of this
        # the python I/O is used to build files.
        local_file_name = "test_tiered19_local_file"
        with open(local_file_name, 'wb') as local_file:
            outbytes = ('MORE THAN ENOUGH DATA\n'*100000).encode()
            local_file.write(outbytes)

        # The object doesn't exist yet.
        self.assertFalse(fs.fs_exist(session, local_file_name))

        # We expect a valid file to flush to GCP.
        self.assertEquals(ss.ss_flush(session, fs, local_file_name, local_file_name, None), 0)
        self.assertEquals(ss.ss_flush_finish(session, fs, local_file_name, local_file_name, None), 0)


        # The object exists now.
        self.assertTrue(fs.fs_exist(session, local_file_name))

        # Open existing file in the cloud. Only one active file handle exists for each open file.
        # A reference count keeps track of open file instances so we can get a pointer to the same
        # file handle as long as there are more open file calls than close file calls (i.e. reference
        # count is greater than 0).
        fh_1 = fs.fs_open_file(session, local_file_name, file_system.open_file_type_data, file_system.open_readonly)
        assert(fh_1 != None)
        fh_2 = fs.fs_open_file(session, local_file_name, file_system.open_file_type_data, file_system.open_readonly)
        assert(fh_2 != None)


        # Test directory list is able to find the file.
        self.assertEquals(fs.fs_directory_list(session, '', ''), [local_file_name])

        # File handle lock call not used in GCP implementation.
        self.assertEqual(fh_1.fh_lock(session, True), 0)
        self.assertEqual(fh_1.fh_lock(session, False), 0)


        err_msg = '/Exception: Operation not supported/'

        # Read using a valid file handle.
        inbytes_1 = bytes(1000000)
        self.assertEqual(fh_1.fh_read(session, 0, inbytes_1), 0)



        # Test that POSIX Remove and Rename are not supported.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: fs.fs_remove(session, 'foobar', 0), err_msg)
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: fs.fs_rename(session, 'foobar', 'foobar2', 0), err_msg)

        # Close a valid file handle.
        self.assertEqual(fh_1.close(session), 0)


        # Read using a valid file handle.
        inbytes_2 = bytes(1000000)
        self.assertEqual(fh_2.fh_read(session, 0, inbytes_2), 0)
        self.assertEquals(outbytes[0:1000000], inbytes_2)

        # File size succeeds.
        self.assertEqual(fh_2.fh_size(session), 2200000)

        # Close a valid file handle.
        self.assertEqual(fh_2.close(session), 0)


        # Test directory listing.

        # Create a second file in storage.
        new_file_name = local_file_name + "1"
        self.assertEquals(ss.ss_flush(session, fs, local_file_name, new_file_name, None), 0)
        self.assertEquals(ss.ss_flush_finish(session, fs, local_file_name, new_file_name, None), 0)

        test_files = {f for f in [local_file_name, new_file_name]}

        file_list = fs.fs_directory_list_single(session, '', '')
        self.assertEquals(len(file_list), 1)
        self.assertIn(file_list[0], test_files)

        file_list = fs.fs_directory_list(session, '', '')
        self.assertSetEqual(set(file_list), test_files)


        # We expect an exception to be raised when flushing a file that does not exist.
        err_msg = "/Exception: No such file or directory/"
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_flush(session, fs, 'non_existing_file', 'non_existing_file', None), err_msg)
        # Check that file does not exist in GCP.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_flush_finish(session, fs, 'non_existing_file', 'non_existing_file', None), err_msg)

        # Check the file size is returned.
        self.assertEquals(fs.fs_size(session, local_file_name), len(outbytes))


        fs.terminate(session)
        ss.terminate(session)

    def test_ss_azure_file_system(self):
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
        self.ignoreStderrPatternIfExists('Bucket not specified')

        bad_bucket = "./bucket_BAD"
        err_msg = '/Exception: No such file or directory/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_customize_file_system(
                session, bad_bucket, None, self.get_fs_config(prefix_1)), err_msg)
        self.ignoreStderrPatternIfExists('No such bucket')

        # Test the customize file system function works when there is a valid bucket.
        azure_fs = ss.ss_customize_file_system(
            session, self.bucket, None, self.get_fs_config(prefix_1))

        # Create another file systems to make sure that terminate works.
        ss.ss_customize_file_system(
            session, self.bucket, None, self.get_fs_config(prefix_2))

        # The object doesn't exist yet.
        try:
            exists = azure_fs.fs_exist(session, 'foobar')
        except:
            self.assertEquals(azure_fs.fs_exist(session, 'foobar'), -1)
        self.assertFalse(exists)
        self.ignoreStderrPatternIfExists('does not exist in Azure')

        # We cannot use the file system to create files, it is readonly.
        # So use python I/O to build up the file.
        with open('foobar', 'wb') as f:
            outbytes = ('MORE THAN ENOUGH DATA\n'*100000).encode()
            f.write(outbytes)

        # The object still doesn't exist yet.
        try:
            exists = azure_fs.fs_exist(session, 'foobar')
        except:
            self.assertEquals(azure_fs.fs_exist(session, 'foobar'), -1)
        self.assertFalse(exists)
        self.ignoreStderrPatternIfExists('does not exist in Azure')

        # Flush valid file into Azure.
        self.assertEqual(ss.ss_flush(session, azure_fs, 'foobar', 'foobar', None), 0)
        # Check that file exists in Azure.
        self.assertEqual(ss.ss_flush_finish(session, azure_fs, 'foobar', 'foobar', None), 0)

        # The object exists now.
        self.assertEquals(azure_fs.fs_directory_list(session, None, None), ['foobar'])
        try:
            exists = azure_fs.fs_exist(session, 'foobar')
        except:
            self.assertEquals(azure_fs.fs_exist(session, 'foobar'), -1)
        self.assertTrue(exists)
        # Check file system exists for an existing object.
        self.assertTrue(azure_fs.fs_exist(session, 'foobar'))

        # Open existing file in the cloud. Only one active file handle exists for each open file.
        # A reference count keeps track of open file instances so we can get a pointer to the same
        # file handle as long as there are more open file calls than close file calls (i.e. reference
        # count is greater than 0).
        fh_1 = azure_fs.fs_open_file(session, 'foobar', file_system.open_file_type_data, file_system.open_readonly)
        assert(fh_1 != None)
        fh_2 = azure_fs.fs_open_file(session, 'foobar', file_system.open_file_type_data, file_system.open_readonly)
        assert(fh_2 != None)

        # File handle lock call not used in Azure implementation.
        self.assertEqual(fh_1.fh_lock(session, True), 0)
        self.assertEqual(fh_1.fh_lock(session, False), 0)

        # Read using a valid file handle.
        inbytes_1 = bytes(1000000)
        self.assertEqual(fh_1.fh_read(session, 0, inbytes_1), 0)
        self.assertEquals(outbytes[0:1000000], inbytes_1)

        # Close a valid file handle.
        self.assertEqual(fh_1.close(session), 0)

        # Read using a valid file handle.
        inbytes_2 = bytes(1000000)
        self.assertEqual(fh_2.fh_read(session, 0, inbytes_2), 0)
        self.assertEquals(outbytes[0:1000000], inbytes_2)

        # File size succeeds.
        self.assertEqual(fh_1.fh_size(session), 2200000)

        # Close a valid file handle.
        self.assertEquals(fh_2.close(session), 0)

        # Test that opening invalid file fails.
        bad_file = 'bad_file'
        err_msg = '/Exception: Invalid argument/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: azure_fs.fs_open_file(session, bad_file,
                file_system.open_file_type_data,file_system.open_readonly), err_msg)

        err_msg = '/Exception: No such file or directory/'
        # Flush non valid file into Azure will result in an exception.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: ss.ss_flush(session, azure_fs, 'non_existing_file', 'non_existing_file', None), err_msg)

        # Check that file does not exist in Azure.
        self.assertEqual(ss.ss_flush_finish(session, azure_fs, 'non_existing_file', 'non_existing_file', None), 0)
        self.ignoreStderrPatternIfExists('does not exist in Azure')

        # Test that the no new objects exist after failed flush.
        self.assertEquals(azure_fs.fs_directory_list(session, None, None), ['foobar'])

        err_msg = '/Exception: Operation not supported/'

        # Test that POSIX Remove and Rename are not supported.
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: azure_fs.fs_remove(session, 'foobar', 0), err_msg)
        self.assertEquals(azure_fs.fs_directory_list(session, None, None), ['foobar'])

        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: azure_fs.fs_rename(session, 'foobar', 'foobar2', 0), err_msg)
        self.assertEquals(azure_fs.fs_directory_list(session, None, None), ['foobar'])

        # Flush second valid file into Azure.
        self.assertEqual(ss.ss_flush(session, azure_fs, 'foobar', 'foobar2', None), 0)
        self.ignoreStdoutPatternIfExists('HTTP status code 201 won\'t be retried.')
        # Check that second file exists in Azure.
        self.assertEqual(ss.ss_flush_finish(session, azure_fs, 'foobar', 'foobar2', None), 0)

        # Directory list should show 2 objects in Azure.
        self.assertEquals(azure_fs.fs_directory_list(session, None, None), ['foobar', 'foobar2'])

        # Directory list single should show 1 object.
        self.assertEquals(azure_fs.fs_directory_list_single(session, None, None), ['foobar'])

        # Verify that file system size returns the size in bytes of the 'foobar' object.
        self.assertEquals(azure_fs.fs_size(session, 'foobar'), len(outbytes))

        # Open existing file in the cloud. Only one active file handle exists for each open file.
        # A reference count keeps track of open file instances so we can get a pointer to the same
        # file handle as long as there are more open file calls than close file calls (i.e. reference
        # count is greater than 0).
        fh_1 = azure_fs.fs_open_file(session, 'foobar', file_system.open_file_type_data, file_system.open_readonly)
        assert(fh_1 != None)
        fh_2 = azure_fs.fs_open_file(session, 'foobar', file_system.open_file_type_data, file_system.open_readonly)
        assert(fh_2 != None)

        # File handle lock call not used in Azure implementation.
        self.assertEqual(fh_1.fh_lock(session, True), 0)
        self.assertEqual(fh_1.fh_lock(session, False), 0)

        # Read using a valid file handle.
        inbytes_1 = bytes(1000000)
        self.assertEqual(fh_1.fh_read(session, 0, inbytes_1), 0)
        self.assertEquals(outbytes[0:1000000], inbytes_1)

        # Close a valid file handle.
        self.assertEqual(fh_1.close(session), 0)

        # Read using a valid file handle.
        inbytes_2 = bytes(1000000)
        self.assertEqual(fh_2.fh_read(session, 0, inbytes_2), 0)
        self.assertEquals(outbytes[0:1000000], inbytes_2)

        # File size succeeds.
        self.assertEqual(fh_1.fh_size(session), 2200000)

        # Close a valid file handle.
        self.assertEquals(fh_2.close(session), 0)

        # Test that opening invalid file fails.
        bad_file = 'bad_file'
        err_msg = '/Exception: Invalid argument/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda: azure_fs.fs_open_file(session, bad_file,
                file_system.open_file_type_data,file_system.open_readonly), err_msg)

        # Test that azure file system terminate succeeds.
        self.assertEqual(azure_fs.terminate(session), 0)

        # Test that azure storage source terminate succeeds.
        self.assertEqual(ss.terminate(session), 0)
        self.ignoreStdoutPatternIfExists('HTTP status code 2')
