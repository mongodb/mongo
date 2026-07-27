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

import compatibility_test, compatibility_version, os, wiredtiger

class test_disagg_stepup(compatibility_test.CompatibilityTestCase):
    '''
    Test a node stepping up as leader on disaggregated checkpoints written by an older release.

    Older releases do not record the write generation high-water mark in the checkpoint
    metadata, so a node picking such checkpoints up cannot adopt the mark and must derive it
    from its local metadata when it becomes leader. Without it, the old leader's transaction
    ids -- persisted while another transaction was running -- are read as the new leader's own
    current ids, and the rows are invisible to its snapshots.
    '''

    uri = 'layered:test_disagg_stepup'
    create_config = 'key_format=S,value_format=S'
    nitems = 5000

    old_home = 'node_old'
    new_home = 'node_new'
    meta_file = 'checkpoint_meta.txt'

    def disagg_conn_config(self, branch, role):
        ext_path = os.path.join(self.branch_build_path(branch.name),
                                'ext', 'page_log', 'palite', 'libwiredtiger_palite.so')
        return f'create,statistics=(all),extensions=["{ext_path}"],' + \
            f'disaggregated=(page_log=palite,lose_all_my_data=true,role="{role}")'

    def test_disagg_stepup(self):
        # Disaggregated storage first shipped in mongodb-9.0.
        disagg_version = compatibility_version.WTVersion('mongodb-9.0')
        if self.older_branch < disagg_version:
            self.skipTest(f'{self.older_branch.name} does not support disaggregated storage')

        self.run_method_on_branch(self.older_branch, 'populate_on_older_branch')
        self.run_method_on_branch(self.newer_branch, 'step_up_on_newer_branch')

    def populate_on_older_branch(self):
        '''
        The old leader persists rows under a running transaction and leaves its checkpoints
        behind. A role cycle in the middle starts a new run, so the final checkpoint records a
        run write generation well past the small generations of a freshly started node: the new
        leader below cannot recognize the checkpoint as cross-run from its own generations
        alone.
        '''
        os.mkdir(self.old_home)
        conn = wiredtiger.wiredtiger_open(self.old_home,
            self.disagg_conn_config(self.older_branch, 'leader'))
        session = conn.open_session()
        session.create(self.uri, self.create_config)

        # Populate under an open transaction so the populating transactions' ids are persisted
        # rather than cleared.
        pin_session = conn.open_session()
        pin_session.begin_transaction()

        cursor = session.open_cursor(self.uri)
        for i in range(1, self.nitems + 1):
            session.begin_transaction()
            cursor[str(i)] = 'v' * 20
            session.commit_transaction('commit_timestamp=' + f'{10:x}')
        cursor.close()
        conn.set_timestamp('stable_timestamp=' + f'{10:x}')
        session.checkpoint()

        pin_session.rollback_transaction()
        pin_session.close()

        # Step down and back up to start a new run, then checkpoint again.
        conn.reconfigure('disaggregated=(role="follower")')
        conn.reconfigure('disaggregated=(role="leader")')

        cursor = session.open_cursor(self.uri)
        for i in range(1, 100):
            session.begin_transaction()
            cursor[str(i)] = 'w' * 20
            session.commit_transaction('commit_timestamp=' + f'{20:x}')
        cursor.close()
        conn.set_timestamp('stable_timestamp=' + f'{20:x}')
        session.checkpoint()

        # Hand the last complete checkpoint's metadata to the new node.
        page_log = conn.get_page_log('palite')
        (_, _, _, meta) = page_log.pl_get_complete_checkpoint(session)
        page_log.terminate(session)
        with open(self.meta_file, 'w') as f:
            f.write(meta)

        session.close()
        conn.close()

    def step_up_on_newer_branch(self):
        '''
        A new node picks up the old leader's checkpoint and steps up. Every row the old leader
        committed must be visible to it.
        '''
        os.mkdir(self.new_home)
        # Share the page log data written by the old node.
        os.symlink(os.path.join('..', self.old_home, 'kv_home'),
                   os.path.join(self.new_home, 'kv_home'), target_is_directory=True)

        conn = wiredtiger.wiredtiger_open(self.new_home,
            self.disagg_conn_config(self.newer_branch, 'follower'))
        session = conn.open_session()
        session.create(self.uri, self.create_config)

        with open(self.meta_file, 'r') as f:
            meta = f.read()
        conn.reconfigure(f'disaggregated=(checkpoint_meta="{meta}",role="leader")')

        cursor = session.open_cursor(self.uri)
        count = 0
        while cursor.next() == 0:
            count += 1
        cursor.close()
        assert count == self.nitems, f'expected {self.nitems} rows, got {count}'

        # The new leader's own checkpoint completes over the inherited data.
        session.checkpoint()

        session.close()
        conn.close()

if __name__ == '__main__':
    compatibility_test.run()
