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
from test_verbose01 import test_verbose_base
import json

# test_verbose03.py
# Tests that when enabling JSON-encoded messages through the event handler interface, valid JSON
# is produced. Valid messages are those that can be successfully parsed as JSON (meeting the JSON
# standard) and subscribe to an expected schema (i.e. meet expected fields and types).
class test_verbose03(test_verbose_base):
    # The maximum number of lines we will read from stdout/stderr in any given context.
    nlines = 50000

    @contextmanager
    def expect_event_handler_json(self, config, stdErr=False):
        # Clean the stdout/stderr resource before yielding the context to the execution block. We only want to
        # capture the verbose output of the using context (ignoring any previous output up to this point).
        if stdErr:
            self.cleanStderr()
        else:
            self.cleanStdout()
        # Create a new connection with JSON format enabled.
        if stdErr:
            conn_config = 'json_output=[error]'
        else:
            conn_config = 'json_output=[message]'
        if config != "":
            conn_config += "," + config
        conn = self.wiredtiger_open(self.home, conn_config)
        # Yield the connection resource to the execution context, allowing it to perform any necessary
        # operations on the connection (for generating the expected message output).
        yield conn
        # Read the contents of stdout/stderr to extract our messages.
        output = self.readStderr(self.nlines) if stdErr else self.readStdout(self.nlines)
        # Split the output into their individual messages. We want validate the contents of each message
        # to ensure we've only generated JSON messages.
        messages = output.splitlines()

        if len(output) >= self.nlines:
            # If we've read the maximum number of characters, its likely that the last line is truncated ('...'). In this
            # case, trim the last message as we can't parse it.
            messages = messages[:-1]

        # Test the contents of each verbose message, ensuring we can successfully parse the JSON and that is subscribes
        # to the expected schema.
        for line in messages:
            try:
                msg = json.loads(line)
            except Exception as e:
                self.pr('Unable to parse JSON message format: %s' % line)
                raise e
            self.validate_json_schema(msg)

        # Close the connection resource and clean up the contents of the stdout/stderr file, flushing out the
        # verbose output that occurred during the execution of this context.
        conn.close()
        if stdErr:
            self.cleanStderr()
        else:
            self.cleanStdout()

    # Test use cases passing sets of verbose categories, ensuring the verbose messages follow a valid JSON schema.
    def test_verbose_json_message(self):
        # Close the initial connection. We will be opening new connections with different verbosity settings throughout
        # this test.
        self.close_conn()

        # Test passing a single verbose category, 'api'.
        with self.expect_event_handler_json(self.create_verbose_configuration(['api'])) as conn:
            # Perform a set of simple API operations (table creations and cursor operations) to generate verbose API
            # messages.
            uri = 'table:test_verbose03_api'
            session = conn.open_session()
            session.create(uri, 'key_format=S,value_format=S')
            c = session.open_cursor(uri)
            c['api'] = 'api'
            c.close()
            session.close()

        # Test passing multiple verbose categories, being 'api' & 'version'.
        with self.expect_event_handler_json(self.create_verbose_configuration(['api','version'])) as conn:
            # Perform a set of simple API operations (table creations and cursor operations) to generate verbose API
            # messages. Beyond opening the connection resource, we shouldn't need to do anything special for the version
            # category.
            uri = 'table:test_verbose03_multiple'
            session = conn.open_session()
            session.create(uri, 'key_format=S,value_format=S')
            c = session.open_cursor(uri)
            c['multiple'] = 'multiple'
            c.close()

    # Test use cases generating error messages, ensuring the messages follow a valid JSON schema.
    def test_verbose_json_err_message(self):
        # Close the initial connection. We will be opening new connections with different verbosity settings throughout
        # this test.
        self.close_conn()

        # Test generating an error message, ensuring the JSON output is valid.
        with self.expect_event_handler_json('', stdErr=True) as conn:
            # Attempt to begin a read transaction with an invalid timestamp, inorder to produce an error message.
            uri = 'table:test_verbose03_error'
            session = conn.open_session()
            session.create(uri, 'key_format=S,value_format=S')
            c = session.open_cursor(uri)
            try:
                session.begin_transaction('read_timestamp=-1')
            except wiredtiger.WiredTigerError:
                # We intend to generate a WiredTigerError. Catch and move forward.
                pass
            c.close()
            session.close()

if __name__ == '__main__':
    wttest.run()
