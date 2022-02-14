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
from suite_subprocess import suite_subprocess
import wttest
from wtscenario import make_scenarios

# test_schema08.py
#    Test schema operations on recovery.
# Test all schema operations alter, create, drop, rename.
# After doing the operation, create a backup copy of the directory,
# walk the log recording each LSN, truncate the backup copy of the
# log walking backward from the LSNs and then run recovery.
class test_schema08(wttest.WiredTigerTestCase, suite_subprocess):
    # We want to copy, truncate and run recovery so keep the log
    # file small and don't pre-allocate any. We expect a small log.
    conn_config = 'log=(enabled,file_max=100k,prealloc=false,remove=false)'
    types = [
        ('file', dict(uri='file:', use_cg=False, use_index=False)),
        ('lsm', dict(uri='lsm:', use_cg=False, use_index=False)),
        ('table-cg', dict(uri='table:', use_cg=True, use_index=False)),
        ('table-index', dict(uri='table:', use_cg=False, use_index=True)),
        ('table-simple', dict(uri='table:', use_cg=False, use_index=False)),
    ]
    ops = [
        ('none', dict(schema_ops='none')),
        ('alter', dict(schema_ops='alter')),
        ('drop', dict(schema_ops='drop')),
        ('rename', dict(schema_ops='rename')),
    ]
    ckpt = [
        ('no_ckpt', dict(ckpt=False)),
        ('with_ckpt', dict(ckpt=True)),
    ]
    scenarios = make_scenarios(types, ops, ckpt)
    count = 0
    lsns = []
    backup_pfx = "BACKUP."

    def do_alter(self, uri, suburi):
        alter_param = 'cache_resident=true'
        self.session.alter(uri, alter_param)
        if suburi != None:
            self.session.alter(suburi, alter_param)

    def do_ops(self, uri, suburi):
        if (self.schema_ops == 'none'):
            return
        if (self.schema_ops == 'alter'):
            self.do_alter(uri, suburi)
        elif (self.schema_ops == 'drop'):
            self.session.drop(uri, None)
        elif (self.schema_ops == 'rename'):
            newuri = self.uri + "new-table"
            self.session.rename(uri, newuri, None)

    # Count actual log records in the log. Log cursors walk the individual
    # operations of a transaction as well as the entire record. Skip counting
    # any individual commit operations and only count entire records.
    def find_logrecs(self):
        self.count = 0
        self.session.log_flush('sync=on')
        c = self.session.open_cursor('log:', None, None)
        self.lsns.append(0)
        while c.next() == 0:
            # lsn.file, lsn.offset, opcount
            keys = c.get_key()
            # We don't expect to need more than one log file. We only store
            # the offsets in a list so assert lsn.file is 1.
            self.assertTrue(keys[0] == 1)

            # Only count whole records, which is when opcount is zero.
            # If opcount is not zero it is an operation of a commit.
            # Skip LSN 128, that is a system record and its existence
            # is assumed within the system.
            if keys[2] == 0 and keys[1] != 128:
                self.count += 1
                self.lsns.append(keys[1])
        c.close()
        self.pr("Find " + str(self.count) + " logrecs LSNS: ")
        self.pr(str(self.lsns))

    def make_backups(self):
        # With the connection still open, copy files to the new directory.
        # Make an initial copy as well as a copy for each LSN we save.
        # Truncate the log to the appropriate offset as we make each copy.
        olddir = "."
        log1 = 'WiredTigerLog.0000000001'
        for lsn in self.lsns:
            newdir = self.backup_pfx + str(lsn)
            shutil.rmtree(newdir, ignore_errors=True)
            os.mkdir(newdir)
            for fname in os.listdir(olddir):
                fullname = os.path.join(olddir, fname)
                # Skip lock file on Windows since it is locked
                if os.path.isfile(fullname) and \
                    "WiredTiger.lock" not in fullname and \
                    "Tmplog" not in fullname and \
                    "Preplog" not in fullname:
                    shutil.copy(fullname, newdir)
            # Truncate the file to the LSN offset.
            # NOTE: This removes the record at that offset
            # resulting in recovery running to just before
            # that record.
            if lsn != 0:
                logf = os.path.join(newdir + '/' + log1)
                f = open(logf, "r+")
                f.truncate(lsn)
                f.close()
                # print "New size " + logf + ": " + str(os.path.getsize(logf))

    def run_recovery(self, uri, suburi):
        # With the connection still open, copy files to the new directory.
        # Make an initial copy as well as a copy for each LSN we save.
        # Truncate the log to the appropriate offset as we make each copy.
        olddir = "."
        errfile="errfile.txt"
        for lsn in self.lsns:
            newdir = self.backup_pfx + str(lsn)
            outfile = newdir + '.txt'
            self.runWt(['-R', '-h', newdir, 'list', '-v'], errfilename=errfile, outfilename=outfile)
            if os.path.isfile(errfile) and os.path.getsize(errfile) > 0:
                self.check_file_contains(errfile,'No such file or directory')

    # Test that creating and dropping tables does not write individual
    # log records.
    def test_schema08_create(self):
        self.count = 0
        self.lsns = []
        uri = self.uri + 'table0'
        create_params = 'key_format=i,value_format=S,'

        cgparam = ''
        suburi = None
        if self.use_cg or self.use_index:
            cgparam = 'columns=(k,v),'
        if self.use_cg:
            cgparam += 'colgroups=(g0),'

        # Create main table.
        self.session.create(uri, create_params + cgparam)

        # Checkpoint after the main table creation if wanted.
        if self.ckpt:
            self.session.checkpoint()

        # Add in column group or index tables.
        if self.use_cg:
            # Create.
            cgparam = 'columns=(v),'
            suburi = 'colgroup:table0:g0'
            self.session.create(suburi, cgparam)

        if self.use_index:
            # Create.
            suburi = 'index:table0:i0'
            self.session.create(suburi, cgparam)

        self.do_ops(uri, suburi)
        self.find_logrecs()
        # print "Found " + str(self.count) + " log records"
        self.make_backups()
        self.run_recovery(uri, suburi)

if __name__ == '__main__':
    wttest.run()
