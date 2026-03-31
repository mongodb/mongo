"""Unit tests for the buildscripts.resmokelib.logging.handlers module."""

import logging
import os
import tempfile
import threading
import unittest

from buildscripts.resmokelib.logging import flush, handlers


class TestBufferedFileHandler(unittest.TestCase):
    """Unit tests for the BufferedFileHandler class."""

    @classmethod
    def setUpClass(cls):
        flush.start_thread()

    @classmethod
    def tearDownClass(cls):
        flush.stop_thread()

    def setUp(self):
        self.tmpfile = tempfile.NamedTemporaryFile(mode="r", delete=False, suffix=".log")
        self.tmpfile.close()
        self.handler = None

    def tearDown(self):
        if self.handler is not None:
            try:
                self.handler.close()
            except Exception:
                pass
        try:
            os.unlink(self.tmpfile.name)
        except OSError:
            pass

    def _make_handler(self, capacity=100, interval_secs=600):
        self.handler = handlers.BufferedFileHandler(
            self.tmpfile.name, capacity=capacity, interval_secs=interval_secs
        )
        self.handler.setFormatter(logging.Formatter("%(message)s"))
        return self.handler

    def _make_record(self, msg):
        return logging.LogRecord(
            name="test",
            level=logging.INFO,
            pathname="",
            lineno=0,
            msg=msg,
            args=(),
            exc_info=None,
        )

    def _read_output(self):
        with open(self.tmpfile.name) as f:
            return f.read()

    def test_emit_and_close_writes_to_file(self):
        """Records emitted before close() appear in the output file."""
        handler = self._make_handler()
        handler.emit(self._make_record("hello"))
        handler.emit(self._make_record("world"))
        handler.close()
        self.handler = None

        content = self._read_output()
        self.assertIn("hello", content)
        self.assertIn("world", content)

    def test_close_flushes_remaining_buffer(self):
        """Closing the handler flushes any records still in the buffer."""
        handler = self._make_handler()
        handler.emit(self._make_record("buffered line"))
        handler.close()
        self.handler = None

        content = self._read_output()
        self.assertIn("buffered line", content)

    def test_close_closes_file_descriptor(self):
        """After close(), the underlying file descriptor is closed."""
        handler = self._make_handler()
        handler.close()
        self.handler = None
        self.assertTrue(handler.file.closed)

    def test_flush_writes_to_file(self):
        """An explicit flush() writes buffered records to disk."""
        handler = self._make_handler()
        handler.emit(self._make_record("flushed"))
        handler.flush()
        # flush() writes to the Python file buffer; sync to disk.
        handler.file.flush()

        content = self._read_output()
        self.assertIn("flushed", content)

    def test_multiple_flush_calls_are_idempotent(self):
        """Calling flush multiple times does not duplicate output."""
        handler = self._make_handler()
        handler.emit(self._make_record("only once"))
        handler.flush()
        handler.flush()
        handler.flush()
        handler.close()
        self.handler = None

        content = self._read_output()
        self.assertEqual(content.count("only once"), 1)

    def test_concurrent_close_and_flush_does_not_raise(self):
        """Concurrent flush-thread activity and close() must not raise ValueError.

        This test reproduces the race condition where the flush thread has
        grabbed the emit buffer and is about to call _flush_buffer_with_lock(),
        while the main thread calls close() which closes the file descriptor.

        Without the fix, the flush thread's writelines() hits a closed file,
        raising "ValueError: I/O operation on closed file."

        Strategy: use a barrier to force this interleaving:
          1. Flush thread grabs the buffer, enters _flush_buffer_with_lock,
             hits the barrier, and waits.
          2. Main thread calls close(): super().close() runs (final flush gets
             an empty buffer since the flush thread already grabbed it), then
             the main thread hits the barrier before closing the file.
          3. Both threads release from the barrier simultaneously.
          4. Main thread closes the file; flush thread writes to the file.
          5. Without the fix: writelines() on a closed file -> ValueError.
             With the fix: the flush lock in close() serializes properly,
             or the closed-file guard in _flush_buffer_with_lock skips the write.
        """
        handler = self._make_handler(capacity=5000, interval_secs=600)
        errors = []

        for i in range(10):
            handler.emit(self._make_record(f"line {i}"))

        barrier = threading.Barrier(2, timeout=5)

        original_flush_buffer = handler._flush_buffer_with_lock

        def racing_flush_buffer(buf, close_called):
            """On the flush thread's call, pause to let close() race ahead."""
            if not close_called:
                try:
                    barrier.wait()
                except threading.BrokenBarrierError:
                    pass
            original_flush_buffer(buf, close_called)

        handler._flush_buffer_with_lock = racing_flush_buffer

        # We need to intercept the file close to add the barrier sync point.
        # But with the fix, close() acquires __flush_lock before closing the
        # file, so we intercept at the top of BufferedFileHandler.close()
        # instead. We replace close() to:
        # 1. Call super().close() (BufferedHandler.close)
        # 2. Hit the barrier (allowing the flush thread to proceed)
        # 3. Then close the file (racing with the flush thread's write)
        def racing_close():
            # Run the BufferedHandler part of close (cancel + final flush).
            handlers.BufferedHandler.close(handler)
            # Now signal the flush thread that we're about to close the file.
            try:
                barrier.wait()
            except threading.BrokenBarrierError:
                pass
            # Close the file — this races with the flush thread's write.
            handler.file.close()

        def flush_thread_fn():
            try:
                handler.flush()
            except ValueError as e:
                errors.append(e)

        flush_t = threading.Thread(target=flush_thread_fn)
        flush_t.start()

        try:
            racing_close()
        except ValueError as e:
            errors.append(e)
        self.handler = None

        flush_t.join(timeout=5)
        self.assertFalse(flush_t.is_alive(), "Flush thread did not finish in time")
        self.assertEqual(errors, [], f"Race condition caused errors: {errors}")
