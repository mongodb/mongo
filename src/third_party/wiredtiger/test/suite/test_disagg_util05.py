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

import os, re
import wttest
from helper_disagg import disagg_test_class
from run import wt_builddir
from suite_subprocess import suite_subprocess

# Verify that the wt CLI rejects the subcommands and the global flags that are
# not supported in disaggregated storage mode, end-to-end.
@disagg_test_class
class test_disagg_util05(wttest.WiredTigerTestCase, suite_subprocess):
    # Keep in sync with util_func_allowed_disagg() in src/utilities/util_main.c
    ALLOWED = frozenset(('dump', 'list', 'page', 'read', 'stat', 'turtle', 'verify'))
    NO_STORAGE_ACCESS = frozenset(('copyright',))

    # Keep in sync with util_flag_allowed_disagg() in src/utilities/util_main.c
    ALLOWED_FLAGS = frozenset(('-C', '-E', '-h', '-m', '-p', '-q', '-V', '-v', '-?'))
    NO_DISAGG_CHECK_FLAGS = frozenset(('-V', '-?'))
    HARNESS_FLAGS = frozenset(('-C', '-h'))
    FLAG_ARGS = {'-E': 'dummy_key', '-l': 'no-such-live-restore'}

    # wiredtiger_open applies the global options itself and refuses these two for a disaggregated
    # database, so the open fails with the library's own message.
    OPEN_FAILURE_MSGS = {
        '-l': 'Live restore is not compatible with disaggregated storage mode',
        '-r': 'disaggregated storage is not supported with read-only connections',
    }

    REJECT_MSG = 'is not supported in disaggregated storage mode'

    conn_config = 'disaggregated=(role="leader")'

    def _page_log_extension_path(self):
        ext_dir = os.path.join(wt_builddir, 'ext', 'page_log', self.ds_name)
        candidates = [os.path.join(ext_dir, e) for e in os.listdir(ext_dir)
                      if e.endswith('.so') or e.endswith('.dylib')]
        self.assertEqual(len(candidates), 1,
            f"expected exactly one page-log shared object under {ext_dir}, got {candidates}")
        return candidates[0]

    # Step the leader down and return a home and config that a wt subprocess can
    # attach to as a follower. A completed checkpoint is required so the follower
    # attaches cleanly: the rejections are made against the open connection, so
    # the connection has to succeed first.
    def _follower_setup(self, name='wt-follower'):
        self.session.create('layered:test_disagg_util05', 'key_format=S,value_format=S')
        self.session.checkpoint()
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.close_conn()

        follower_home = os.path.join(self.home, name)
        os.mkdir(follower_home)
        os.symlink('../kv_home', os.path.join(follower_home, 'kv_home'),
            target_is_directory=True)

        config = (f'create,'
                  f'extensions=[{self._page_log_extension_path()}],'
                  f'disaggregated=(role="follower",page_log={self.page_log()})')
        return follower_home, config

    # Run `wt -?` and return the name of the file holding its output.
    def _usage_output(self, follower_home, follower_config):
        errfile = 'wt-help.err'
        self.runWt(['-h', follower_home, '-C', follower_config, '-?'],
                    outfilename='wt-help.out', errfilename=errfile, closeconn=False)
        return errfile

    # Parse one section ("global_options:", "commands:") of `wt -?` output for all entry names
    # to test against the names in the rejected list.
    def _usage_section(self, errfile, section):
        names = []
        in_section = False
        with open(errfile) as f:
            for line in f:
                if not line.startswith(' '):
                    in_section = line.startswith(section)
                    continue
                if not in_section:
                    continue
                m = re.match(r'^    (-\S|[a-z_]+)(?:\s|$)', line)
                if m:
                    names.append(m.group(1))
        return names

    def test_reject_subcommands(self):
        follower_home, follower_config = self._follower_setup()

        subcmds = self._usage_section(
            self._usage_output(follower_home, follower_config), 'commands:')
        self.assertGreater(len(subcmds), 0)

        for cmd in subcmds:
            if cmd in self.NO_STORAGE_ACCESS:
                continue
            rejected = cmd not in self.ALLOWED
            self.runWt(['-h', follower_home, '-C', follower_config, cmd, '-?'],
                       outfilename='wt.out', errfilename='wt.err', closeconn=False,
                       failure=rejected)
            if rejected:
                self.check_file_contains('wt.err', self.REJECT_MSG)
            else:
                self.check_file_not_contains('wt.err', self.REJECT_MSG)

    def test_reject_flags(self):
        follower_home, follower_config = self._follower_setup()

        flags = self._usage_section(
            self._usage_output(follower_home, follower_config), 'global_options:')
        self.assertGreater(len(flags), 0)

        for flag in flags:
            if flag in self.NO_DISAGG_CHECK_FLAGS or flag in self.HARNESS_FLAGS:
                continue
            argv = [flag]
            if flag in self.FLAG_ARGS:
                argv.append(self.FLAG_ARGS[flag])
            rejected = flag not in self.ALLOWED_FLAGS
            self.runWt(['-h', follower_home, '-C', follower_config] + argv + ['stat'],
                       outfilename='wt.out', errfilename='wt.err', closeconn=False,
                       failure=rejected)

            if rejected:
                expected = expected = self.OPEN_FAILURE_MSGS.get(flag, f'{flag} {self.REJECT_MSG}')
                self.check_file_contains('wt.err', expected)
            else:
                self.check_file_not_contains('wt.err', self.REJECT_MSG)
