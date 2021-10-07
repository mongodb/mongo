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
#    Test the local storage source's file system implementation.
# Note that the APIs we are testing are not meant to be used directly
# by any WiredTiger application, these APIs are used internally.
# However, it is useful to do tests of this API independently.
class test_tiered06(wttest.WiredTigerTestCase):
    # Load the local store extension.
    def conn_extensions(self, extlist):
        # Windows doesn't support dynamically loaded extension libraries.
        if os.name == 'nt':
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
        return self.conn.get_storage_source('local_store')

    def test_local_basic(self):
        # Test some basic functionality of the storage source API, calling
        # each supported method in the API at least once.

        session = self.session
        local = self.get_local_storage_source()

        os.mkdir("objects")
        fs = local.ss_customize_file_system(session, "./objects", "Secret", None)

        # The object doesn't exist yet.
        self.assertFalse(fs.fs_exist(session, 'foobar'))

        # We cannot use the file system to create files, it is readonly.
        # So use python I/O to build up the file.
        f = open('foobar', 'wb')

        # The object still doesn't exist yet.
        self.assertFalse(fs.fs_exist(session, 'foobar'))

        outbytes = ('MORE THAN ENOUGH DATA\n'*100000).encode()
        f.write(outbytes)
        f.close()

        # Nothing is in the directory list until a flush.
        self.assertEquals(fs.fs_directory_list(session, '', ''), [])

        # Flushing copies the file into the file system.
        local.ss_flush(session, fs, 'foobar', 'foobar', None)
        local.ss_flush_finish(session, fs, 'foobar', 'foobar', None)

        # The object exists now.
        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])
        self.assertTrue(fs.fs_exist(session, 'foobar'))

        fh = fs.fs_open_file(session, 'foobar', FileSystem.open_file_type_data, FileSystem.open_readonly)
        inbytes = bytes(1000000)         # An empty buffer with a million zero bytes.
        fh.fh_read(session, 0, inbytes)  # Read into the buffer.
        self.assertEquals(outbytes[0:1000000], inbytes)
        self.assertEquals(fs.fs_size(session, 'foobar'), len(outbytes))
        self.assertEquals(fh.fh_size(session), len(outbytes))
        fh.close(session)

        # The fh_lock call doesn't do anything in the local store implementation.
        fh = fs.fs_open_file(session, 'foobar', FileSystem.open_file_type_data, FileSystem.open_readonly)
        fh.fh_lock(session, True)
        fh.fh_lock(session, False)
        fh.close(session)

        # Files that have been flushed cannot be manipulated.
        with self.expectedStderrPattern('foobar: rename of file not supported'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: fs.fs_rename(session, 'foobar', 'barfoo', 0))
        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])

        # Files that have been flushed cannot be manipulated through the custom file system.
        with self.expectedStderrPattern('foobar: remove of file not supported'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: fs.fs_remove(session, 'foobar', 0))
        self.assertEquals(fs.fs_directory_list(session, '', ''), ['foobar'])

        fs.terminate(session)
        local.terminate(session)

    def test_local_write_read(self):
        # Write and read to a file non-sequentially.

        session = self.session
        local = self.get_local_storage_source()

        cachedir = ("objects_cache")
        os.mkdir("objects")
        os.mkdir("objects_cache")
        fs = local.ss_customize_file_system(
            session, "./objects", "Secret", "cache_directory=" + cachedir)

        # We call these 4K chunks of data "blocks" for this test, but that doesn't
        # necessarily relate to WT block sizing.
        nblocks = 1000
        block_size = 4096
        f = open('abc', 'wb')

        # Create some blocks filled with 'a', etc.
        a_block = ('a' * block_size).encode()
        b_block = ('b' * block_size).encode()
        c_block = ('c' * block_size).encode()
        file_size = nblocks * block_size

        # Write all blocks as 'a', but in reverse order.
        for pos in range(file_size - block_size, 0, -block_size):
            f.seek(pos)
            f.write(a_block)

        # Write the even blocks as 'b', forwards.
        for pos in range(0, file_size, block_size * 2):
            f.seek(pos)
            f.write(b_block)

        # Write every third block as 'c', backwards.
        for pos in range(file_size - block_size, 0, -block_size * 3):
            f.seek(pos)
            f.write(c_block)
        f.close()

        # Flushing copies the file into the file system.
        local.ss_flush(session, fs, 'abc', 'abc', None)
        local.ss_flush_finish(session, fs, 'abc', 'abc', None)

        # Use the file system to open and read the file.
        # We do this twice, and between iterations, we remove the cached file to make sure
        # it is copied back from the bucket directory.
        #
        # XXX: this uses knowledge of the implementation, but at the current time,
        # we don't have a way via the API to "age out" a file from the cache.
        for i in range(0, 2):
            in_block = bytes(block_size)
            fh = fs.fs_open_file(session, 'abc', FileSystem.open_file_type_data, FileSystem.open_readonly)

            # Do some spot checks, reading non-sequentially.
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
            os.remove(os.path.join(cachedir, 'abc'))

        local.terminate(session)

    def create_with_fs(self, fs, fname):
        session = self.session
        f = open(fname, 'wb')
        f.write('some stuff'.encode())
        f.close()

    objectdir1 = "./objects1"
    objectdir2 = "./objects2"

    cachedir1 = "./cache1"
    cachedir2 = "./cache2"

    # Add a suffix to each in a list.
    def suffix(self, lst, sfx):
        return [x + '.' + sfx for x in lst]

    def check_dirlist(self, fs, prefix, expect):
        # We don't require any sorted output for directory lists,
        # so we'll sort before comparing.'
        got = sorted(fs.fs_directory_list(self.session, '', prefix))
        expect = sorted(self.suffix(expect, 'wtobj'))
        self.assertEquals(got, expect)

    # Check for data files in the WiredTiger home directory.
    def check_home(self, expect):
        # Get list of all .wt files in home, prune out the WiredTiger produced ones.
        got = sorted(list(os.listdir(self.home)))
        got = [x for x in got if not x.startswith('WiredTiger') and x.endswith('.wt')]
        expect = sorted(self.suffix(expect, 'wt'))
        self.assertEquals(got, expect)

    # Check that objects are "in the cloud" after a flush.
    # Using the local storage module, they are actually going to be in either
    # objectdir1 or objectdir2.
    def check_objects(self, expect1, expect2):
        got = sorted(list(os.listdir(self.objectdir1)))
        expect = sorted(self.suffix(expect1, 'wtobj'))
        self.assertEquals(got, expect)
        got = sorted(list(os.listdir(self.objectdir2)))
        expect = sorted(self.suffix(expect2, 'wtobj'))
        self.assertEquals(got, expect)

    # Check that objects are in the cache directory after flush_finish.
    def check_caches(self, expect1, expect2):
        got = sorted(list(os.listdir(self.cachedir1)))
        expect = sorted(self.suffix(expect1, 'wtobj'))
        self.assertEquals(got, expect)
        got = sorted(list(os.listdir(self.cachedir2)))
        expect = sorted(self.suffix(expect2, 'wtobj'))
        self.assertEquals(got, expect)

    def create_wt_file(self, name):
        with open(name + '.wt', 'w') as f:
            f.write('hello')

    def test_local_file_systems(self):
        # Test using various buckets, hosts.

        session = self.session
        local = self.conn.get_storage_source('local_store')
        self.local = local
        os.mkdir(self.objectdir1)
        os.mkdir(self.objectdir2)
        os.mkdir(self.cachedir1)
        os.mkdir(self.cachedir2)
        config1 = "cache_directory=" + self.cachedir1
        config2 = "cache_directory=" + self.cachedir2
        bad_config = "cache_directory=/BAD"

        # Create file system objects. First try some error cases.
        errmsg = '/No such file or directory/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: local.ss_customize_file_system(
                session, "./objects1", "k1", bad_config), errmsg)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: local.ss_customize_file_system(
                session, "./objects_BAD", "k1", config1), errmsg)

        # Create an empty file, try to use it as a directory.
        with open("some_file", "w"):
            pass
        errmsg = '/Invalid argument/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: local.ss_customize_file_system(
                session, "some_file", "k1", config1), errmsg)

        # Now create some file systems that should succeed.
        # Use either different bucket directories or different prefixes,
        # so activity that happens in the various file systems should be independent.
        fs1 = local.ss_customize_file_system(session, "./objects1", "k1", config1)
        fs2 = local.ss_customize_file_system(session, "./objects2", "k2", config2)

        # Create files in the wt home directory.
        for a in ['beagle', 'bird', 'bison', 'bat']:
            self.create_wt_file(a)
        for a in ['cat', 'cougar', 'coyote', 'cub']:
            self.create_wt_file(a)

        # Everything is in wt home, nothing in the file system yet.
        self.check_home(['beagle', 'bird', 'bison', 'bat', 'cat', 'cougar', 'coyote', 'cub'])
        self.check_dirlist(fs1, '', [])
        self.check_dirlist(fs2, '', [])
        self.check_caches([], [])
        self.check_objects([], [])

        # A flush copies to the cloud, nothing is removed.
        local.ss_flush(session, fs1, 'beagle.wt', 'beagle.wtobj')
        self.check_home(['beagle', 'bird', 'bison', 'bat', 'cat', 'cougar', 'coyote', 'cub'])
        self.check_dirlist(fs1, '', ['beagle'])
        self.check_dirlist(fs2, '', [])
        self.check_caches([], [])
        self.check_objects(['beagle'], [])

        # Bad file to flush.
        errmsg = '/No such file/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: local.ss_flush(session, fs1, 'bad.wt', 'bad.wtobj'), errmsg)

        # It's okay to flush again, nothing changes.
        local.ss_flush(session, fs1, 'beagle.wt', 'beagle.wtobj')
        self.check_home(['beagle', 'bird', 'bison', 'bat', 'cat', 'cougar', 'coyote', 'cub'])
        self.check_dirlist(fs1, '', ['beagle'])
        self.check_dirlist(fs2, '', [])
        self.check_caches([], [])
        self.check_objects(['beagle'], [])

        # When we flush_finish, the local file will be in both the local and cache directory.
        local.ss_flush_finish(session, fs1, 'beagle.wt', 'beagle.wtobj')
        self.check_home(['beagle', 'bird', 'bison', 'bat', 'cat', 'cougar', 'coyote', 'cub'])
        self.check_dirlist(fs1, '', ['beagle'])
        self.check_dirlist(fs2, '', [])
        self.check_caches(['beagle'], [])
        self.check_objects(['beagle'], [])

        # Do a some more in each file system.
        local.ss_flush(session, fs1, 'bison.wt', 'bison.wtobj')
        local.ss_flush(session, fs2, 'cat.wt', 'cat.wtobj')
        local.ss_flush(session, fs1, 'bat.wt', 'bat.wtobj')
        local.ss_flush_finish(session, fs2, 'cat.wt', 'cat.wtobj')
        local.ss_flush(session, fs2, 'cub.wt', 'cub.wtobj')
        local.ss_flush_finish(session, fs1, 'bat.wt', 'bat.wtobj')

        self.check_home(['beagle', 'bird', 'bison', 'bat', 'cat', 'cougar', 'coyote', 'cub'])
        self.check_dirlist(fs1, '', ['beagle', 'bat', 'bison'])
        self.check_dirlist(fs2, '', ['cat', 'cub'])
        self.check_caches(['beagle', 'bat'], ['cat'])
        self.check_objects(['beagle', 'bat', 'bison'], ['cat', 'cub'])

        # Test directory listing prefixes.
        self.check_dirlist(fs1, '', ['beagle', 'bat', 'bison'])
        self.check_dirlist(fs1, 'ba', ['bat'])
        self.check_dirlist(fs1, 'be', ['beagle'])
        self.check_dirlist(fs1, 'x', [])

        # Terminate just one of the custom file systems.
        # We should be able to terminate file systems, but we should
        # also be able to terminate the storage source without terminating
        # all the file systems we created.
        fs1.terminate(session)
        local.terminate(session)

if __name__ == '__main__':
    wttest.run()
