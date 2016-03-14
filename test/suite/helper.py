#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

# python has a filecmp.cmp function, but different versions of python approach
# file comparison differently.  To make sure we get byte for byte comparison,
# we define it here.
def compare_files(self, filename1, filename2):
    self.pr('compare_files: ' + filename1 + ', ' + filename2)
    bufsize = 4096
    if os.path.getsize(filename1) != os.path.getsize(filename2):
        print 'file comparison failed: ' + filename1 + ' size ' +\
            str(os.path.getsize(filename1)) + ' != ' + filename2 +\
            ' size ' + str(os.path.getsize(filename2))
        return False
    with open(filename1, "rb") as fp1:
        with open(filename2, "rb") as fp2:
            while True:
                b1 = fp1.read(bufsize)
                b2 = fp2.read(bufsize)
                if b1 != b2:
                    return False
                # files are identical size
                if not b1:
                    return True

# Iterate over a set of tables, ensuring that they have identical contents
def compare_tables(self, session, uris, config=None):
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

# confirm a URI doesn't exist.
def confirm_does_not_exist(self, uri):
    self.pr('confirm_does_not_exist: ' + uri)
    self.assertRaises(wiredtiger.WiredTigerError,
        lambda: self.session.open_cursor(uri, None))
    self.assertEqual(glob.glob('*' + uri.split(":")[-1] + '*'), [],
        'confirm_does_not_exist: URI exists, file name matching \"' +
        uri.split(":")[1] + '\" found')

# confirm a URI exists and is empty.
def confirm_empty(self, uri):
    self.pr('confirm_empty: ' + uri)
    cursor = self.session.open_cursor(uri, None)
    if cursor.value_format == '8t':
        for key,val in cursor:
            self.assertEqual(val, 0)
    else:
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
    cursor.close()

# copy a WT home directory
def copy_wiredtiger_home(olddir, newdir, aligned=True):
    # unaligned copy requires 'dd', which may not be available on Windows
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
                shutil.copy(fullname, newdir)
            else:
                fullname = os.path.join(olddir, fname)
                inpf = 'if=' + fullname
                outf = 'of=' + newdir + '/' + fullname
                cmd_list = ['dd', inpf, outf, 'bs=300']
                a = subprocess.Popen(cmd_list)
                a.wait()

# create a simple_populate or complex_populate key
def key_populate(cursor, i):
    key_format = cursor.key_format
    if key_format == 'i' or key_format == 'r' or key_format == 'u':
        return i
    elif key_format == 'S':
        return str('%015d' % i)
    else:
        raise AssertionError(
            'key_populate: object has unexpected format: ' + key_format)

# create a simple_populate value
def value_populate(cursor, i):
    value_format = cursor.value_format
    if value_format == 'i' or value_format == 'r' or value_format == 'u':
        return i
    elif value_format == 'S':
        return str(i) + ': abcdefghijklmnopqrstuvwxyz'
    elif value_format == '8t':
        value = (
            0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xaa, 0xab,
            0xac, 0xad, 0xae, 0xaf, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
            0xb7, 0xb8, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf)
        return value[i % len(value)]
    else:
        raise AssertionError(
            'value_populate: object has unexpected format: ' + value_format)

# population of a simple object
#    uri:       object
#    config:    prefix of the session.create configuration string (defaults
#               to string value formats)
#    rows:      entries to insert
def simple_populate(self, uri, config, rows):
    self.pr('simple_populate: ' + uri + ' with ' + str(rows) + ' rows')
    self.session.create(uri, 'value_format=S,' + config)
    cursor = self.session.open_cursor(uri, None)
    for i in range(1, rows + 1):
        cursor[key_populate(cursor, i)] = value_populate(cursor, i)
    cursor.close()

def simple_populate_check_cursor(self, cursor, rows):
    i = 0
    for key,val in cursor:
        i += 1
        self.assertEqual(key, key_populate(cursor, i))
        if cursor.value_format == '8t' and val == 0:    # deleted
            continue
        self.assertEqual(val, value_populate(cursor, i))
    self.assertEqual(i, rows)

def simple_populate_check(self, uri, rows):
    self.pr('simple_populate_check: ' + uri)
    cursor = self.session.open_cursor(uri, None)
    simple_populate_check_cursor(self, cursor, rows)
    cursor.close()

# Return the value stored in a complex object.
def complex_value_populate(cursor, i):
    return [str(i) + ': abcdefghijklmnopqrstuvwxyz'[0:i%26],
        i,
        str(i) + ': abcdefghijklmnopqrstuvwxyz'[0:i%23],
        str(i) + ': abcdefghijklmnopqrstuvwxyz'[0:i%18]]

# Return the number of column groups used
def complex_populate_colgroup_count():
    return 6

# Return the number of indices used
def complex_populate_index_count():
    return 6

# population of a complex object
#    uri:       object
#    config:    prefix of the session.create configuration string
#    rows:      entries to insert
def complex_populate(self, uri, config, rows):
        complex_populate_type(self, uri, config, '', rows, '')
def complex_populate_cgconfig(self, uri, config, rows):
        complex_populate_type(self, uri, config, config, rows, '')
def complex_populate_lsm(self, uri, config, rows):
        complex_populate_type(self, uri, config, '', rows, 'type=lsm')
def complex_populate_cgconfig_lsm(self, uri, config, rows):
        complex_populate_type(self, uri, config, config, rows, 'type=lsm')
def complex_populate_type(self, uri, config, cgconfig, rows, type):
    self.session.create(uri,
        config + ',value_format=SiSS,' +
        'columns=(record,column2,column3,column4,column5),' +
        'colgroups=(cgroup1,cgroup2,cgroup3,cgroup4,cgroup5,cgroup6)')

    cgname = 'colgroup:' + uri.split(":")[1]
    cgcfg = ',' + cgconfig + ',' + type
    self.session.create(cgname + ':cgroup1', 'columns=(column2)' + ',' + cgcfg)
    self.session.create(cgname + ':cgroup2', 'columns=(column3)' + ',' + cgcfg)
    self.session.create(cgname + ':cgroup3', 'columns=(column4)' + ',' + cgcfg)
    self.session.create(
        cgname + ':cgroup4', 'columns=(column2,column3)' + ',' + cgcfg)
    self.session.create(
        cgname + ':cgroup5', 'columns=(column3,column4)' + ',' + cgcfg)
    self.session.create(
        cgname + ':cgroup6', 'columns=(column2,column4,column5)' + ',' + cgcfg)
    indxname = 'index:' + uri.split(":")[1]
    self.session.create(indxname + ':indx1', 'columns=(column2)' + ',' + cgcfg)
    self.session.create(indxname + ':indx2', 'columns=(column3)' + ',' + cgcfg)
    self.session.create(indxname + ':indx3', 'columns=(column4)' + ',' + cgcfg)
    self.session.create(
        indxname + ':indx4', 'columns=(column2,column4)' + ',' + cgcfg)
    cursor = self.session.open_cursor(uri, None)
    for i in range(1, rows + 1):
        cursor[key_populate(cursor, i)] = \
                tuple(complex_value_populate(cursor, i))
    cursor.close()
    # add some indices after populating
    self.session.create(
        indxname + ':indx5', 'columns=(column3,column5)' + ',' + cgcfg)
    self.session.create(
        indxname + ':indx6', 'columns=(column3,column5,column4)' + ',' + cgcfg)

def complex_populate_colgroup_name(self, uri, i):
    return 'colgroup:' + uri.split(":")[1] + ':cgroup' + str(i + 1)

def complex_populate_index_name(self, uri, i):
    return 'index:' + uri.split(":")[1] + ':indx' + str(i + 1)

def complex_populate_check_cursor(self, cursor, rows):
    i = 0
    for key, s1, i2, s3, s4 in cursor:
        i += 1
        self.assertEqual(key, key_populate(cursor, i))
        v = complex_value_populate(cursor, i)
        self.assertEqual(s1, v[0])
        self.assertEqual(i2, v[1])
        self.assertEqual(s3, v[2])
        self.assertEqual(s4, v[3])
    self.assertEqual(i, rows)

def complex_populate_check(self, uri, rows):
    self.pr('complex_populate_check: ' + uri)
    cursor = self.session.open_cursor(uri, None)
    complex_populate_check_cursor(self, cursor, rows)
    cursor.close()
