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

import os, wiredtiger, wttest
FileSystem = wiredtiger.FileSystem  # easy access to constants

# test_tiered06.py
#    Test the local storage source.
class test_tiered06(wttest.WiredTigerTestCase):
    # Load the local store extension, but skip the test if it is missing.
    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        #extlist.extension('storage_sources',
        #  'local_store=(config=\"(verbose=1,delay_ms=200,force_delay=3)\")')
        extlist.extension('storage_sources', 'local_store')

    def breakpoint(self):
        import pdb, sys
        sys.stdin = open('/dev/tty', 'r')
        sys.stdout = open('/dev/tty', 'w')
        sys.stderr = open('/dev/tty', 'w')
        pdb.set_trace()

    def get_local_storage_source(self):
        local = self.conn.get_storage_source('local_store')

        # Note: do not call local.terminate() .
        # Since the local_storage extension has been loaded as a consequence of the
        # wiredtiger_open call, WiredTiger already knows to call terminate when the connection
        # closes.  Calling it twice would attempt to free the same memory twice.
        local.terminate = None
        return local

    def test_local_basic(self):
        # Test some basic functionality of the storage source API, calling
        # each supported method in the API at least once.

        session = self.session
        local = self.get_local_storage_source()

        os.mkdir("objects")
        fs = local.ss_customize_file_system(session, "./objects", "cluster1-", "Secret", None)

        # The object doesn't exist yet.
        self.assertFalse(fs.fs_exist(session, 'foobar'))

        fh = fs.fs_open_file(session, 'foobar', FileSystem.open_file_type_data, FileSystem.open_create)

        # Just like a regular file system, the object exists now.
        self.assertTrue(fs.fs_exist(session, 'foobar'))

        outbytes = ('MORE THAN ENOUGH DATA\n'*100000).encode()
        fh.fh_write(session, 0, outbytes)

        # The object exists after close
        fh.close(session)
        self.assertTrue(fs.fs_exist(session, 'foobar'))

        fh = fs.fs_open_file(session, 'foobar', FileSystem.open_file_type_data, FileSystem.open_readonly)
        inbytes = bytes(1000000)         # An empty buffer with a million zero bytes.
        fh.fh_read(session, 0, inbytes)  # read into the buffer
        self.assertEquals(outbytes[0:1000000], inbytes)
        self.assertEquals(fs.fs_size(session, 'foobar'), len(outbytes))
        self.assertEquals(fh.fh_size(session), len(outbytes))
        fh.close(session)

        # The fh_lock call doesn't do anything in the local store implementation.
        fh = fs.fs_open_file(session, 'foobar', FileSystem.open_file_type_data, FileSystem.open_readonly)
        fh.fh_lock(session, True)
        fh.fh_lock(session, False)
        fh.close(session)

        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])

        # Newly created objects are in the list.
        fh = fs.fs_open_file(session, 'zzz', FileSystem.open_file_type_data, FileSystem.open_create)

        # TODO: tiered: the newly created file should be visible, but it is not yet.
        #  self.assertEquals(sorted(fs.fs_directory_list(session, '', '')), ['foobar', 'zzz' ])

        # Sync merely syncs to the local disk.
        fh.fh_sync(session)
        fh.close(session)    # zero length
        self.assertEquals(sorted(fs.fs_directory_list(session, '', '')), ['foobar', 'zzz' ])

        # See that we can rename objects.
        fs.fs_rename(session, 'zzz', 'yyy', 0)
        self.assertEquals(sorted(fs.fs_directory_list(session, '', '')), ['foobar', 'yyy' ])

        # See that we can remove objects.
        fs.fs_remove(session, 'yyy', 0)
        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])

        # TODO: tiered: flush tests disabled, as the interface
        # for flushing will be changed.
        return

        # Flushing doesn't do anything that's visible.
        local.ss_flush(session, fs, None, '')
        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])

        # Files that have been flushed cannot be manipulated.
        with self.expectedStderrPattern('foobar: rename of flushed file not allowed'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: fs.fs_rename(session, 'foobar', 'barfoo', 0))
        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])

        # Files that have been flushed cannot be manipulated through the custom file system.
        with self.expectedStderrPattern('foobar: remove of flushed file not allowed'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: fs.fs_remove(session, 'foobar', 0))
        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])

        fs.terminate(session)

    def test_local_write_read(self):
        # Write and read to a file non-sequentially.

        session = self.session
        local = self.get_local_storage_source()

        os.mkdir("objects")
        fs = local.ss_customize_file_system(session, "./objects", "cluster1-", "Secret", None)

        # We call these 4K chunks of data "blocks" for this test, but that doesn't
        # necessarily relate to WT block sizing.
        nblocks = 1000
        block_size = 4096
        fh = fs.fs_open_file(session, 'abc', FileSystem.open_file_type_data, FileSystem.open_create)

        # blocks filled with 'a', etc.
        a_block = ('a' * block_size).encode()
        b_block = ('b' * block_size).encode()
        c_block = ('c' * block_size).encode()
        file_size = nblocks * block_size

        # write all blocks as 'a', but in reverse order
        for pos in range(file_size - block_size, 0, -block_size):
            fh.fh_write(session, pos, a_block)

        # write the even blocks as 'b', forwards
        for pos in range(0, file_size, block_size * 2):
            fh.fh_write(session, pos, b_block)

        # write every third block as 'c', backwards
        for pos in range(file_size - block_size, 0, -block_size * 3):
            fh.fh_write(session, pos, c_block)
        fh.close(session)

        in_block = bytes(block_size)
        fh = fs.fs_open_file(session, 'abc', FileSystem.open_file_type_data, FileSystem.open_readonly)

        # Do some spot checks, reading non-sequentially
        fh.fh_read(session, 500 * block_size, in_block)  # divisible by 2, not 3
        self.assertEquals(in_block, b_block)
        fh.fh_read(session, 333 * block_size, in_block)  # divisible by 3, not 2
        self.assertEquals(in_block, c_block)
        fh.fh_read(session, 401 * block_size, in_block)  # not divisible by 2 or 3
        self.assertEquals(in_block, a_block)

        # Read the whole file, backwards checking to make sure
        # each block was written correctly.
        for block_num in range(nblocks - 1, 0, -1):
            pos = block_num * block_size
            fh.fh_read(session, pos, in_block)
            if block_num % 3 == 0:
                self.assertEquals(in_block, c_block)
            elif block_num % 2 == 0:
                self.assertEquals(in_block, b_block)
            else:
                self.assertEquals(in_block, a_block)
        fh.close(session)

    def create_with_fs(self, fs, fname):
        session = self.session
        fh = fs.fs_open_file(session, fname, FileSystem.open_file_type_data, FileSystem.open_create)
        fh.fh_write(session, 0, 'some stuff'.encode())
        fh.close(session)

    objectdir1 = "./objects1"
    objectdir2 = "./objects2"

    cachedir1 = "./cache1"
    cachedir2 = "./cache2"

    def check(self, fs, prefix, expect):
        # We don't require any sorted output for directory lists,
        # so we'll sort before comparing.'
        got = sorted(fs.fs_directory_list(self.session, '', prefix))
        expect = sorted(expect)
        self.assertEquals(got, expect)

    # Check that objects are "in the cloud" after a flush.
    # Using the local storage module, they are actually going to be in either
    # objectdir1 or objectdir2
    def check_objects(self, expect1, expect2):
        got = sorted(list(os.listdir(self.objectdir1)))
        expect = sorted(expect1)
        self.assertEquals(got, expect)
        got = sorted(list(os.listdir(self.objectdir2)))
        expect = sorted(expect2)
        self.assertEquals(got, expect)

    def test_local_file_systems(self):
        # Test using various buckets, hosts

        session = self.session
        local = self.conn.get_storage_source('local_store')
        self.local = local
        os.mkdir(self.objectdir1)
        os.mkdir(self.objectdir2)
        os.mkdir(self.cachedir1)
        os.mkdir(self.cachedir2)
        config1 = "cache_directory=" + self.cachedir1
        config2 = "cache_directory=" + self.cachedir2
        bad_config = "cache_directory=BAD"

        # Create file system objects. First try some error cases.
        errmsg = '/No such file or directory/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: local.ss_customize_file_system(
                session, "./objects1", "pre1-", "k1", bad_config), errmsg)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: local.ss_customize_file_system(
                session, "./objects_BAD", "pre1-", "k1", config1), errmsg)

        # Create an empty file, try to use it as a directory.
        with open("some_file", "w"):
            pass
        errmsg = '/Invalid argument/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: local.ss_customize_file_system(
                session, "some_file", "pre1-", "k1", config1), errmsg)

        # Now create some file systems that should succeed.
        # Use either different bucket directories or different prefixes,
        # so activity that happens in the various file systems should be independent.
        fs1 = local.ss_customize_file_system(session, "./objects1", "pre1-", "k1", config1)
        fs2 = local.ss_customize_file_system(session, "./objects2", "pre1-", "k2", config2)
        fs3 = local.ss_customize_file_system(session, "./objects1", "pre2-", "k3", config1)
        fs4 = local.ss_customize_file_system(session, "./objects2", "pre2-", "k4", config2)

        # Create files in the file systems with some name overlap
        self.create_with_fs(fs1, 'alpaca')
        self.create_with_fs(fs2, 'bear')
        self.create_with_fs(fs3, 'crab')
        self.create_with_fs(fs4, 'deer')
        for a in ['beagle', 'bird', 'bison', 'bat']:
            self.create_with_fs(fs1, a)
        for a in ['bird', 'bison', 'bat', 'badger']:
            self.create_with_fs(fs2, a)
        for a in ['bison', 'bat', 'badger', 'baboon']:
            self.create_with_fs(fs3, a)
        for a in ['bat', 'badger', 'baboon', 'beagle']:
            self.create_with_fs(fs4, a)

        # Make sure we see the expected file names
        self.check(fs1, '', ['alpaca', 'beagle', 'bird', 'bison', 'bat'])
        self.check(fs1, 'a', ['alpaca'])
        self.check(fs1, 'b', ['beagle', 'bird', 'bison', 'bat'])
        self.check(fs1, 'c', [])
        self.check(fs1, 'd', [])

        self.check(fs2, '', ['bear', 'bird', 'bison', 'bat', 'badger'])
        self.check(fs2, 'a', [])
        self.check(fs2, 'b', ['bear', 'bird', 'bison', 'bat', 'badger'])
        self.check(fs2, 'c', [])
        self.check(fs2, 'd', [])

        self.check(fs3, '', ['crab', 'bison', 'bat', 'badger', 'baboon'])
        self.check(fs3, 'a', [])
        self.check(fs3, 'b', ['bison', 'bat', 'badger', 'baboon'])
        self.check(fs3, 'c', ['crab'])
        self.check(fs3, 'd', [])

        self.check(fs4, '', ['deer', 'bat', 'badger', 'baboon', 'beagle'])
        self.check(fs4, 'a', [])
        self.check(fs4, 'b', ['bat', 'badger', 'baboon', 'beagle'])
        self.check(fs4, 'c', [])
        self.check(fs4, 'd', ['deer'])

        # Flushing copies files to one of the subdirectories:
        #    "./objects1" (for fs1 and fs3)
        #    "./objects2" (for fs2 and fs4)
        #
        # After every flush, we'll check that the right objects appear in the right directory.
        # check_objects takes two lists: objects expected to be in ./objects1,
        # and objects expected to be in ./objects2 .
        self.check_objects([], [])

        # TODO: tiered: flush tests disabled, as the interface
        # for flushing will be changed.
        enable_fs_flush_tests = False
        if enable_fs_flush_tests:
            local.ss_flush(session, fs4, None, '')
            self.check_objects([], ['pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            local.ss_flush(session, fs3, 'badger', '')
            self.check_objects(['pre2-badger'],
                               ['pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            #local.ss_flush(session, fs3, 'c', '')     # make sure we don't flush prefixes
            self.check_objects(['pre2-badger'],
                               ['pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            local.ss_flush(session, fs3, 'b', '')     # or suffixes
            self.check_objects(['pre2-badger'],
                               ['pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            local.ss_flush(session, fs3, 'crab', '')
            self.check_objects(['pre2-crab', 'pre2-badger'],
                               ['pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            local.ss_flush(session, fs3, 'crab', '')  # should do nothing
            self.check_objects(['pre2-crab', 'pre2-badger'],
                               ['pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            local.ss_flush(session, None, None, '')   # flush everything else
            self.check_objects(['pre1-alpaca', 'pre1-beagle', 'pre1-bird', 'pre1-bison', 'pre1-bat',
                                'pre2-crab', 'pre2-bison', 'pre2-bat', 'pre2-badger', 'pre2-baboon'],
                               ['pre1-bear', 'pre1-bird', 'pre1-bison', 'pre1-bat', 'pre1-badger',
                                'pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            local.ss_flush(session, None, None, '')   # should do nothing
            self.check_objects(['pre1-alpaca', 'pre1-beagle', 'pre1-bird', 'pre1-bison', 'pre1-bat',
                                'pre2-crab', 'pre2-bison', 'pre2-bat', 'pre2-badger', 'pre2-baboon'],
                               ['pre1-bear', 'pre1-bird', 'pre1-bison', 'pre1-bat', 'pre1-badger',
                                'pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            self.create_with_fs(fs4, 'zebra')         # should do nothing in the objects directories
            self.create_with_fs(fs4, 'yeti')          # should do nothing in the objects directories
            self.check_objects(['pre1-alpaca', 'pre1-beagle', 'pre1-bird', 'pre1-bison', 'pre1-bat',
                                'pre2-crab', 'pre2-bison', 'pre2-bat', 'pre2-badger', 'pre2-baboon'],
                               ['pre1-bear', 'pre1-bird', 'pre1-bison', 'pre1-bat', 'pre1-badger',
                                'pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle'])

            # Try remove and rename, should be possible until we flush
            self.check(fs4, '', ['deer', 'bat', 'badger', 'baboon', 'beagle', 'yeti', 'zebra'])
            fs4.fs_remove(session, 'yeti', 0)
            self.check(fs4, '', ['deer', 'bat', 'badger', 'baboon', 'beagle', 'zebra'])
            fs4.fs_rename(session, 'zebra', 'okapi', 0)
            self.check(fs4, '', ['deer', 'bat', 'badger', 'baboon', 'beagle', 'okapi'])
            local.ss_flush(session, None, None, '')
            self.check(fs4, '', ['deer', 'bat', 'badger', 'baboon', 'beagle', 'okapi'])
            self.check_objects(['pre1-alpaca', 'pre1-beagle', 'pre1-bird', 'pre1-bison', 'pre1-bat',
                                'pre2-crab', 'pre2-bison', 'pre2-bat', 'pre2-badger', 'pre2-baboon'],
                               ['pre1-bear', 'pre1-bird', 'pre1-bison', 'pre1-bat', 'pre1-badger',
                                'pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle',
                                'pre2-okapi'])

            errmsg = '/rename of flushed file not allowed/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                                         lambda: fs4.fs_rename(session, 'okapi', 'zebra', 0), errmsg)

            # XXX
            # At the moment, removal of flushed files is not allowed - as flushed files are immutable.
            # We may need to explicitly evict flushed files from cache directory via the API, if so,
            # the API to do that might be on the local store object, not the file system.
            errmsg = '/remove of flushed file not allowed/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                                         lambda: fs4.fs_remove(session, 'okapi', 0), errmsg)

            # No change since last time.
            self.check(fs4, '', ['deer', 'bat', 'badger', 'baboon', 'beagle', 'okapi'])
            self.check_objects(['pre1-alpaca', 'pre1-beagle', 'pre1-bird', 'pre1-bison', 'pre1-bat',
                                'pre2-crab', 'pre2-bison', 'pre2-bat', 'pre2-badger', 'pre2-baboon'],
                               ['pre1-bear', 'pre1-bird', 'pre1-bison', 'pre1-bat', 'pre1-badger',
                                'pre2-deer', 'pre2-bat', 'pre2-badger', 'pre2-baboon', 'pre2-beagle',
                                'pre2-okapi'])

if __name__ == '__main__':
    wttest.run()
