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

import glob, os, shutil, string, subprocess
import wiredtiger

from wtdataset import SimpleDataSet, SimpleIndexDataSet, ComplexDataSet

# Python has a filecmp.cmp function, but different versions of python approach
# file comparison differently.  To make sure we get byte for byte comparison,
# we define it here.
def compare_files(testcase, filename1, filename2):
    testcase.pr('compare_files: ' + filename1 + ', ' + filename2)
    bufsize = 4096
    if os.path.getsize(filename1) != os.path.getsize(filename2):
        print('file comparison failed: ' + filename1 + ' size ' +\
            str(os.path.getsize(filename1)) + ' != ' + filename2 +\
            ' size ' + str(os.path.getsize(filename2)))
        return False
    with open(filename1, "rb") as fp1:
        with open(filename2, "rb") as fp2:
            while True:
                b1 = fp1.read(bufsize)
                b2 = fp2.read(bufsize)
                if b1 != b2:
                    return False
                # Files are identical size.
                if not b1:
                    return True

# Iterate over a set of tables, ensuring that they have identical contents
def compare_tables(testcase, session, uris, config=None):
    cursors = list()
    for next_uri in uris:
        cursors.append(session.open_cursor(next_uri, None, config))

    try:
        done = False
        while not done:
            keys = list()
            for next_cursor in cursors:
                if (next_cursor.next() == wiredtiger.WT_NOTFOUND):
                    done = True
                    break
                keys.append(next_cursor.get_value())
            match = all(x == keys[0] for x in keys)
            if not match:
                return False

        return True
    finally:
        for c in cursors:
            c.close()

# Confirm a URI doesn't exist.
def confirm_does_not_exist(testcase, uri):
    testcase.pr('confirm_does_not_exist: ' + uri)
    testcase.assertRaises(wiredtiger.WiredTigerError,
        lambda: testcase.session.open_cursor(uri, None))
    testcase.assertEqual(glob.glob('*' + uri.split(":")[-1] + '*'), [],
        'confirm_does_not_exist: URI exists, file name matching \"' +
        uri.split(":")[1] + '\" found')

# Confirm a URI exists and is empty.
def confirm_empty(testcase, uri):
    testcase.pr('confirm_empty: ' + uri)
    cursor = testcase.session.open_cursor(uri, None)
    if cursor.value_format == '8t':
        for key,val in cursor:
            testcase.assertEqual(val, 0)
    else:
        testcase.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
    cursor.close()

# Copy a WT home directory.
def copy_wiredtiger_home(testcase, olddir, newdir, aligned=True):
    # Unaligned copy requires 'dd', which may not be available on Windows.
    if not aligned and os.name == "nt":
        raise AssertionError(
            'copy_wiredtiger_home: unaligned copy impossible on Windows')
    shutil.rmtree(newdir, ignore_errors=True)
    os.mkdir(newdir)
    for fname in os.listdir(olddir):
        fullname = os.path.join(olddir, fname)
        # Skip lock file, on Windows it is locked.
        # Skip temporary log files.
        if os.path.isfile(fullname) and "WiredTiger.lock" not in fullname and \
            "WiredTigerTmplog" not in fullname and \
            "WiredTigerPreplog" not in fullname:
            # Use a dd command that does not align on a block boundary.
            if aligned:
                try:
                    shutil.copy(fullname, newdir)
                except OSError as e:
                    # If the copy failed it means the file was removed underneath us in the
                    # short time after we verified the files existence. This can happen if we
                    # are copying wiredtiger log files at the same time they are getting
                    # removed by the background log server thread. To avoid failing in this case
                    # we can log/print the removed file and continue.
                    if "WiredTigerLog" not in fullname:
                        raise e
                    else:
                        testcase.pr('Skipping logfile %s: No longer exists' % fname)
                        continue
            else:
                fullname = os.path.join(olddir, fname)
                inpf = 'if=' + fullname
                outf = 'of=' + newdir + '/' + fullname
                cmd_list = ['dd', inpf, outf, 'bs=300']
                a = subprocess.Popen(cmd_list)
                a.wait()

# Simulate a crash from olddir and restart in newdir.
def simulate_crash_restart(testcase, olddir, newdir):
    # With the connection still open, copy files to new directory.
    copy_wiredtiger_home(testcase, olddir, newdir)
    # Close the original connection and open to new directory.
    # NOTE:  This really cannot test the difference between the
    # write-no-sync (off) version of log_flush and the sync
    # version since we're not crashing the system itself.
    testcase.close_conn()
    testcase.conn = testcase.setUpConnectionOpen(newdir)
    testcase.session = testcase.setUpSessionOpen(testcase.conn)
