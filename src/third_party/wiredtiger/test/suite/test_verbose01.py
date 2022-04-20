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

from suite_subprocess import suite_subprocess
from contextlib import contextmanager
import wiredtiger, wttest
import re

# Shared base class used by verbose tests.
class test_verbose_base(wttest.WiredTigerTestCase, suite_subprocess):
    # The maximum number of lines we will read from stdout in any given context.
    nlines = 30000

    def create_verbose_configuration(self, categories):
        if len(categories) == 0:
            return ''
        return 'verbose=[' + ','.join(categories) + ']'

    @contextmanager
    def expect_verbose(self, categories, patterns, expect_output = True):
        # Clean the stdout resource before yielding the context to the execution block. We only want to
        # capture the verbose output of the using context (ignoring any previous output up to this point).
        self.cleanStdout()
        # Create a new connection with the given verbose categories.
        verbose_config = self.create_verbose_configuration(categories)
        conn = self.wiredtiger_open(self.home, verbose_config)
        # Yield the connection resource to the execution context, allowing it to perform any necessary
        # operations on the connection (for generating the expected verbose output).
        yield conn
        # Read the contents of stdout to extract our verbose messages.
        output = self.readStdout(self.nlines)
        # Split the output into their individual messages. We want validate the contents of each message
        # to ensure we've only generated verbose messages for the expected categories.
        verbose_messages = output.splitlines()

        if expect_output:
            self.assertGreater(len(verbose_messages), 0)
        else:
            self.assertEqual(len(verbose_messages), 0)

        if len(output) >= self.nlines:
            # If we've read the maximum number of characters, its likely that the last line is truncated ('...'). In this
            # case, trim the last message as we can't parse it.
            verbose_messages = verbose_messages[:-1]

        # Test the contents of each verbose message, ensuring it satisfies the expected pattern.
        verb_pattern = re.compile('|'.join(patterns))
        for line in verbose_messages:
            self.assertTrue(verb_pattern.search(line) != None, 'Unexpected verbose message: ' + line)

        # Close the connection resource and clean up the contents of the stdout file, flushing out the
        # verbose output that occurred during the execution of this context.
        conn.close()
        self.cleanStdout()

# test_verbose01.py
# Verify basic uses of the verbose configuration API work as intended i.e. passing
# single & multiple valid and invalid verbose categories. These tests are mainly focused on uses
# of the interface prior to the introduction of verbosity levels, ensuring 'legacy'-style
# uses of the interface are still supported.
class test_verbose01(test_verbose_base):
    collection_cfg = 'key_format=S,value_format=S'
    # Test use cases passing single verbose categories, ensuring we only produce verbose output for the single category.
    def test_verbose_single(self):
        # Close the initial connection. We will be opening new connections with different verbosity settings throughout
        # this test.
        self.close_conn()

        # Test passing a single verbose category, 'api'. Ensuring the only verbose output generated is related to
        # the 'api' category.
        with self.expect_verbose(['api'], ['WT_VERB_API']) as conn:
            # Perform a set of simple API operations (table creations and cursor operations) to generate verbose API
            # messages.
            uri = 'table:test_verbose01_api'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            c = session.open_cursor(uri)
            c['api'] = 'api'
            c.close()
            session.close()

        # Test passing another single verbose category, 'compact'. Ensuring the only verbose output generated is related to
        # the 'compact' category.
        with self.expect_verbose(['compact'], ['WT_VERB_COMPACT']) as conn:
            # Create a simple table to invoke compaction on. We aren't doing anything interesting with the table
            # such that the data source will be compacted. Rather we want to simply invoke a compaction pass to
            # generate verbose messages.
            uri = 'table:test_verbose01_compact'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            session.compact(uri)
            session.close()

    # Test use cases passing multiple verbose categories, ensuring we only produce verbose output for specified categories.
    def test_verbose_multiple(self):
        self.close_conn()
        # Test passing multiple verbose categories, being 'api' & 'version'. Ensuring the only verbose output generated
        # is related to those two categories.
        with self.expect_verbose(['api','version'], ['WT_VERB_API', 'WT_VERB_VERSION']) as conn:
            # Perform a set of simple API operations (table creations and cursor operations) to generate verbose API
            # messages. Beyond opening the connection resource, we shouldn't need to do anything special for the version
            # category.
            uri = 'table:test_verbose01_multiple'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            c = session.open_cursor(uri)
            c['multiple'] = 'multiple'
            c.close()

    # Test use cases passing no verbose categories, ensuring we don't produce unexpected verbose output.
    def test_verbose_none(self):
        self.close_conn()
        # Testing passing an empty set of categories. Ensuring no verbose output is generated.
        with self.expect_verbose([], [], False) as conn:
            # Perform a set of simple API operations (table creations and cursor operations). Ensuring no verbose messages
            # are generated.
            uri = 'table:test_verbose01_none'
            session = conn.open_session()
            session.create(uri, self.collection_cfg)
            c = session.open_cursor(uri)
            c['none'] = 'none'
            c.close()

    # Test use cases passing invalid verbose categories, ensuring the appropriate error message is
    # raised.
    def test_verbose_invalid(self):
        self.close_conn()
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
                lambda:self.wiredtiger_open(self.home, 'verbose=[test_verbose_invalid]'),
                '/\'test_verbose_invalid\' not a permitted choice for key \'verbose\'/')

if __name__ == '__main__':
    wttest.run()
