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

import os, re, string
from suite_subprocess import suite_subprocess
import itertools, wiredtiger, wttest

from helper import complex_populate_cgconfig, complex_populate_cgconfig_lsm
from helper import simple_populate
from helper import complex_populate_check, simple_populate_check
from wtscenario import multiply_scenarios, number_scenarios

# test_util13.py
#    Utilities: wt dump, as well as the dump cursor
#    Test that dump and load retain table configuration information.
#
class test_util13(wttest.WiredTigerTestCase, suite_subprocess):
    """
    Test wt dump.  We check for specific output and preservation of
    non-default table create parameters.
    """

    pfx = 'test_util13'
    nentries = 100
    dir = "dump_dir"
    #
    # Select table configuration settings that are not the default.
    #
    types = [
        ('file-simple', dict(uri='file:' + pfx, pop=simple_populate,
            populate_check=simple_populate_check,
            table_config='prefix_compression_min=3', cfg='')),
        ('lsm-simple', dict(uri='lsm:' + pfx, pop=simple_populate,
            populate_check=simple_populate_check,
            table_config='lsm=(bloom_bit_count=29)',
            cfg='bloom_bit_count=29')),
        ('table-simple', dict(uri='table:' + pfx, pop=simple_populate,
            populate_check=simple_populate_check,
            table_config='split_pct=50', cfg='')),
        ('table-complex',
            dict(uri='table:' + pfx, pop=complex_populate_cgconfig,
            populate_check=complex_populate_check,
            table_config='allocation_size=512B', cfg='')),
        ('table-complex-lsm',
            dict(uri='table:' + pfx, pop=complex_populate_cgconfig_lsm,
            populate_check=complex_populate_check,
            table_config='lsm=(merge_max=5)',
            cfg='merge_max=5')),
    ]

    scenarios = number_scenarios(multiply_scenarios('.', types))

    def compare_config(self, expected_cfg, actual_cfg):
        # Replace '(' characters so configuration groups don't break parsing.
        # If we ever want to look for config groups this will need to change.
        #print "compare_config Actual config "
        #print actual_cfg
        #print "compare_config Expected config "
        #print expected_cfg
        cfg_orig = actual_cfg
        if self.pop != simple_populate:
            #
            # If we have a complex config, strip out the colgroups and
            # columns from the config.  Doing so allows us to keep the
            # split commands below usable because those two items don't
            # have assignments in them.
            #
            nocolgrp = re.sub("colgroups=\((.+?)\),", '', actual_cfg)
            cfg_orig = re.sub("columns=\((.+?)\),", '', nocolgrp)

        #print "Using original config "
        #print cfg_orig
        da = dict(kv.split('=') for kv in
            cfg_orig.strip().replace('(',',').split(','))
        dx = dict(kv.split('=') for kv in
            expected_cfg.strip().replace('(',',').split(','))

        # Check that all items in our expected config subset are in
        # the actual configuration and they match.
        match = all(item in da.items() for item in dx.items())
        if match == False:
            print "MISMATCH:"
            print "Original dict: "
            print da
            print "Expected config: "
            print dx
        return match

    def compare_files(self, expect_subset, dump_out):
        inheader = isconfig = False
        for l1, l2 in zip(open(expect_subset, "rb"), open(dump_out, "rb")):
            if isconfig:
                if not self.compare_config(l1, l2):
                    return False
            if inheader:
                # This works because the expected subset has a format
                # of URI and config lines alternating.
                isconfig = not isconfig
            if l1.strip() == 'Header':
                inheader = True
            if l1.strip() == 'Data':
                break
        return True

    def load_recheck(self, expect_subset, dump_out):
        newdump = "newdump.out"
        os.mkdir(self.dir)
        self.runWt(['-h', self.dir, 'load', '-f', dump_out])
        # Check the contents
        conn = self.wiredtiger_open(self.dir)
        session = conn.open_session()
        cursor = session.open_cursor(self.uri, None, None)
        self.populate_check
        conn.close()
        dumpargs = ["-h"]
        dumpargs.append(self.dir)
        dumpargs.append("dump")
        dumpargs.append(self.uri)
        self.runWt(dumpargs, outfilename=newdump)

        self.assertTrue(self.compare_files(expect_subset, newdump))
        return True

    def test_dump_config(self):
        # The number of btree_entries reported is influenced by the
        # number of column groups and indices.  Each insert will have
        # a multiplied effect.
        self.pop(self, self.uri,
            'key_format=S,value_format=S,' + self.table_config, self.nentries)

        ver = wiredtiger.wiredtiger_version()
        verstring = str(ver[1]) + '.' + str(ver[2]) + '.' + str(ver[3])
        expectfile="expect.out"
        with open(expectfile, "w") as expectout:
            # Note: this output is sensitive to the precise output format
            # generated by wt dump.  If this is likely to change, we should
            # make this test more accommodating.
            expectout.write(
                'WiredTiger Dump (WiredTiger Version ' + verstring + ')\n')
            expectout.write('Format=print\n')
            expectout.write('Header\n')
            expectout.write(self.uri + '\n')
            # Check the config on the colgroup itself for complex tables.
            if self.pop != simple_populate:
                expectout.write('key_format=S\n')
                expectout.write('colgroup:' + self.pfx + ':cgroup1\n')
            if self.cfg == '':
                expectout.write(self.table_config + '\n')
            else:
                expectout.write(self.cfg + '\n')
            expectout.write('Data\n')

        self.pr('calling dump')
        outfile="dump.out"
        dumpargs = ["dump"]
        dumpargs.append(self.uri)
        self.runWt(dumpargs, outfilename=outfile)

        self.assertTrue(self.compare_files(expectfile, outfile))
        self.assertTrue(self.load_recheck(expectfile, outfile))

if __name__ == '__main__':
    wttest.run()
