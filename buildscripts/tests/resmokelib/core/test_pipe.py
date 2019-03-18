"""Unit tests for buildscripts/resmokelib/core/pipe.py."""

from __future__ import absolute_import

import io
import logging
import unittest

import mock

from buildscripts.resmokelib.core import pipe as _pipe

# pylint: disable=missing-docstring


class TestLoggerPipe(unittest.TestCase):
    LOG_LEVEL = logging.DEBUG

    @classmethod
    def _get_log_calls(cls, output):
        logger = logging.Logger("for_testing")
        logger.log = mock.MagicMock()

        logger_pipe = _pipe.LoggerPipe(logger=logger, level=cls.LOG_LEVEL,
                                       pipe_out=io.BytesIO(output))
        logger_pipe.wait_until_started()
        logger_pipe.wait_until_finished()

        return logger.log.call_args_list

    def test_strips_trailing_whitespace(self):
        calls = self._get_log_calls(b" a ")
        self.assertEqual(calls, [mock.call(self.LOG_LEVEL, u" a")])

    def test_strips_trailing_newlines(self):
        calls = self._get_log_calls(b"a\r\n")
        self.assertEqual(calls, [mock.call(self.LOG_LEVEL, u"a")])

    def test_handles_invalid_utf8(self):
        calls = self._get_log_calls(b"a\x80b")
        self.assertEqual(calls, [mock.call(self.LOG_LEVEL, u"a\ufffdb")])

    def test_escapes_null_bytes(self):
        calls = self._get_log_calls(b"a\0b")
        self.assertEqual(calls, [mock.call(self.LOG_LEVEL, u"a\\0b")])
