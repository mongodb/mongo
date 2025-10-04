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
from wtscenario import make_scenarios
import json, re, wiredtiger, wttest

# Shared base class used by verbose tests.
class test_verbose_base(wttest.WiredTigerTestCase, suite_subprocess):
    # The maximum number of lines we will read from stdout in any given context.
    nlines = 50000

    # The JSON schema we expect all messages to follow. Captures all possible fields, detailing
    # each field's name, associated type and whether we always expect for that field to be
    # present.
    expected_json_schema = {
        'category': {'type': str, 'always_expected': True },
        'category_id': {'type': int, 'always_expected': True },
        'log_id': {'type': int, 'always_expected': True },
        'error_str': {'type': str, 'always_expected': False },
        'error_code': {'type': int, 'always_expected': False },
        'msg': {'type': str, 'always_expected': True },
        'session_dhandle_name': {'type': str, 'always_expected': False },
        'session_err_prefix': {'type': str, 'always_expected': False },
        'session_name': {'type': str, 'always_expected': False },
        'thread': {'type': str, 'always_expected': True },
        'ts_sec': {'type': int, 'always_expected': True },
        'ts_usec': {'type': int, 'always_expected': True },
        'verbose_level': {'type': str, 'always_expected': True },
        'verbose_level_id': {'type': int, 'always_expected': True },
    }

    # Validates the JSON schema of a given event handler message, ensuring the schema is consistent and expected.
    def validate_json_schema(self, json_msg):
        expected_schema = dict(self.expected_json_schema)

        for field in json_msg:
            # Assert the JSON field is valid and expected.
            self.assertTrue(field in expected_schema, 'Unexpected field "%s" in JSON message: %s' % (field, str(json_msg)))

            # Assert the type of the JSON field is expected.
            self.assertEqual(type(json_msg[field]), expected_schema[field]['type'],
                    'Unexpected type of field "%s" in JSON message, expected "%s" but got "%s": %s' % (field,
                        str(expected_schema[field]['type']), str(type(json_msg[field])), str(json_msg)))

            expected_schema.pop(field, None)

        # Go through the remaining fields in the schema and ensure we've seen all the fields that are always expected be present
        # in the JSON message
        for field in expected_schema:
            self.assertFalse(expected_schema[field]['always_expected'], 'Expected field "%s" in JSON message, but not found: %s' %
                (field, str(json_msg)))

    # Validates the verbose category (and ID) in a JSON message is expected.
    def validate_json_category(self, json_msg, expected_categories):
        # Assert the category field is in the JSON message.
        self.assertTrue('category' in json_msg, 'JSON message missing "category" field')
        self.assertTrue('category_id' in json_msg, 'JSON message missing "category_id" field')
        # Assert the category field values in the JSON message are expected.
        self.assertTrue(json_msg['category'] in expected_categories, 'Unexpected verbose category "%s"' % json_msg['category'])
        self.assertTrue(json_msg['category_id'] == expected_categories[json_msg['category']],
                'The category ID received in the message "%d" does not match its expected definition "%d"' % (json_msg['category_id'], expected_categories[json_msg['category']]))

    def create_verbose_configuration(self, categories):
        if len(categories) == 0:
            return ''
        return 'verbose=[' + ','.join(categories) + ']'

    @contextmanager
    def expect_verbose(self, categories, patterns, expect_json, expect_output = True):
        # Clean the stdout resource before yielding the context to the execution block. We only want to
        # capture the verbose output of the using context (ignoring any previous output up to this point).
        self.cleanStdout()
        # Create a new connection with the given verbose categories.
        verbose_config = self.create_verbose_configuration(categories)
        # Enable JSON output if required.
        if expect_json:
            verbose_config += ",json_output=[message]"
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
        # To avoid truncated messages, slice out the last message string in the
        for line in verbose_messages:
            # Check JSON validity
            if expect_json:
                try:
                    json.loads(line)
                except Exception as e:
                    self.prout('Unable to parse JSON message: %s' % line)
                    raise e

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

    format = [
        ('flat', dict(is_json=False)),
        ('json', dict(is_json=True)),
    ]
    scenarios = make_scenarios(format)

    collection_cfg = 'key_format=S,value_format=S'

    # Test use cases passing single verbose categories, ensuring we only produce verbose output for the single category.
    @wttest.skip_for_hook("tiered", "Fails with tiered storage")
    def test_verbose_single(self):
        # Close the initial connection. We will be opening new connections with different verbosity settings throughout
        # this test.
        self.close_conn()

        # Test passing a single verbose category, 'api'. Ensuring the only verbose output generated is related to
        # the 'api' category.
        with self.expect_verbose(['api'], ['WT_VERB_API'], self.is_json) as conn:
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
        with self.expect_verbose(['compact'], ['WT_VERB_COMPACT'], self.is_json) as conn:
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
        with self.expect_verbose(['api','version'], ['WT_VERB_API', 'WT_VERB_VERSION'], self.is_json) as conn:
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
        with self.expect_verbose([], [], self.is_json, False) as conn:
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
