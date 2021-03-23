#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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
StorageSource = wiredtiger.StorageSource  # easy access to constants

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
        location = local.ss_location_handle(session,
            'cluster="cluster1",bucket="./objects",auth_token="Secret"')

        # The object doesn't exist yet.
        self.assertFalse(local.ss_exist(session, location, 'foobar'))

        fh = local.ss_open_object(session, location, 'foobar', StorageSource.open_create)

        outbytes = ('MORE THAN ENOUGH DATA\n'*100000).encode()
        fh.fh_write(session, 0, outbytes)

        # The object doesn't even exist now.
        self.assertFalse(local.ss_exist(session, location, 'foobar'))

        # The object exists after close
        fh.close(session)
        self.assertTrue(local.ss_exist(session, location, 'foobar'))

        fh = local.ss_open_object(session, location, 'foobar', StorageSource.open_readonly)
        inbytes = bytes(1000000)         # An empty buffer with a million zero bytes.
        fh.fh_read(session, 0, inbytes)  # read into the buffer
        self.assertEquals(outbytes[0:1000000], inbytes)
        self.assertEquals(local.ss_size(session, location, 'foobar'), len(outbytes))
        self.assertEquals(fh.fh_size(session), len(outbytes))
        fh.close(session)

        # The fh_lock call doesn't do anything in the local store implementation.
        fh = local.ss_open_object(session, location, 'foobar', StorageSource.open_readonly)
        fh.fh_lock(session, True)
        fh.fh_lock(session, False)
        fh.close(session)

        self.assertEquals(local.ss_location_list(session, location, '', 0), ['foobar'])

        # Make sure any new object is not in the list until it is closed.
        fh = local.ss_open_object(session, location, 'zzz', StorageSource.open_create)
        self.assertEquals(local.ss_location_list(session, location, '', 0), ['foobar'])
        # Sync merely syncs to the local disk.
        fh.fh_sync(session)
        fh.close(session)    # zero length
        self.assertEquals(sorted(local.ss_location_list(session, location, '', 0)),
          ['foobar', 'zzz'])

        # See that we can remove objects.
        local.ss_remove(session, location, 'zzz', 0)
        self.assertEquals(local.ss_location_list(session, location, '', 0), ['foobar'])

        # Flushing doesn't do anything that's visible.
        local.ss_flush(session, location, None, '')
        self.assertEquals(local.ss_location_list(session, location, '', 0), ['foobar'])

        location.close(session)

    def test_local_write_read(self):
        # Write and read to a file non-sequentially.

        session = self.session
        local = self.get_local_storage_source()

        os.mkdir("objects")
        location = local.ss_location_handle(session,
            'cluster="cluster1",bucket="./objects",auth_token="Secret"')

        # We call these 4K chunks of data "blocks" for this test, but that doesn't
        # necessarily relate to WT block sizing.
        nblocks = 1000
        block_size = 4096
        fh = local.ss_open_object(session, location, 'abc', StorageSource.open_create)

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
        fh = local.ss_open_object(session, location, 'abc', StorageSource.open_readonly)

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

    def create_in_loc(self, loc, objname):
        session = self.session
        fh = self.local.ss_open_object(session, loc, objname, StorageSource.open_create)
        fh.fh_write(session, 0, 'some stuff'.encode())
        fh.close(session)

    def check(self, loc, prefix, limit, expect):
        # We don't require any sorted output for location lists,
        # so we'll sort before comparing.'
        got = sorted(self.local.ss_location_list(self.session, loc, prefix, limit))
        expect = sorted(expect)
        self.assertEquals(got, expect)

    def test_local_locations(self):
        # Test using various buckets, clusters

        session = self.session
        local = self.conn.get_storage_source('local_store')
        self.local = local
        os.mkdir("objects1")
        os.mkdir("objects2")

        # Any of the activity that happens in the various locations
        # should be independent.
        location1 = local.ss_location_handle(session,
            'cluster="cluster1",bucket="./objects1",auth_token="k1"')
        location2 = local.ss_location_handle(session,
            'cluster="cluster1",bucket="./objects2",auth_token="k2"')
        location3 = local.ss_location_handle(session,
            'cluster="cluster2",bucket="./objects1",auth_token="k3"')
        location4 = local.ss_location_handle(session,
            'cluster="cluster2",bucket="./objects2",auth_token="k4"')

        # Create files in the locations with some name overlap
        self.create_in_loc(location1, 'alpaca')
        self.create_in_loc(location2, 'bear')
        self.create_in_loc(location3, 'crab')
        self.create_in_loc(location4, 'deer')
        for a in ['beagle', 'bird', 'bison', 'bat']:
            self.create_in_loc(location1, a)
        for a in ['bird', 'bison', 'bat', 'badger']:
            self.create_in_loc(location2, a)
        for a in ['bison', 'bat', 'badger', 'baboon']:
            self.create_in_loc(location3, a)
        for a in ['bat', 'badger', 'baboon', 'beagle']:
            self.create_in_loc(location4, a)

        # Make sure we see the expected file names
        self.check(location1, '', 0, ['alpaca', 'beagle', 'bird', 'bison', 'bat'])
        self.check(location1, 'a', 0, ['alpaca'])
        self.check(location1, 'b', 0, ['beagle', 'bird', 'bison', 'bat'])
        self.check(location1, 'c', 0, [])
        self.check(location1, 'd', 0, [])

        self.check(location2, '', 0, ['bear', 'bird', 'bison', 'bat', 'badger'])
        self.check(location2, 'a', 0, [])
        self.check(location2, 'b', 0, ['bear', 'bird', 'bison', 'bat', 'badger'])
        self.check(location2, 'c', 0, [])
        self.check(location2, 'd', 0, [])

        self.check(location3, '', 0, ['crab', 'bison', 'bat', 'badger', 'baboon'])
        self.check(location3, 'a', 0, [])
        self.check(location3, 'b', 0, ['bison', 'bat', 'badger', 'baboon'])
        self.check(location3, 'c', 0, ['crab'])
        self.check(location3, 'd', 0, [])

        self.check(location4, '', 0, ['deer', 'bat', 'badger', 'baboon', 'beagle'])
        self.check(location4, 'a', 0, [])
        self.check(location4, 'b', 0, ['bat', 'badger', 'baboon', 'beagle'])
        self.check(location4, 'c', 0, [])
        self.check(location4, 'd', 0, ['deer'])

        # Flushing doesn't do anything that's visible, but calling it still exercises code paths.
        # At some point, we'll have statistics we can check.
        #
        # For now, we can turn on the verbose config option for the local_store extension to verify.
        local.ss_flush(session, location4, None, '')
        local.ss_flush(session, location3, 'badger', '')
        local.ss_flush(session, location3, 'c', '')     # make sure we don't flush prefixes
        local.ss_flush(session, location3, 'b', '')     # or suffixes
        local.ss_flush(session, location3, 'crab', '')
        local.ss_flush(session, location3, 'crab', '')  # should do nothing
        local.ss_flush(session, None, None, '')         # flush everything else
        local.ss_flush(session, None, None, '')         # should do nothing

if __name__ == '__main__':
    wttest.run()
