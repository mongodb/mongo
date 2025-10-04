"""Manage a thread responsible for periodically calling flush() on logging.Handler instances.

These instances are used to send logs to ??? TODO: SERVER-97556
"""

import logging
import sched
import threading
import time

_FLUSH_THREAD_LOCK = threading.Lock()
_FLUSH_THREAD: "_FlushThread" = None


def start_thread():
    """Start the flush thread."""

    global _FLUSH_THREAD
    with _FLUSH_THREAD_LOCK:
        if _FLUSH_THREAD is not None:
            raise ValueError("FlushThread has already been started")

        _FLUSH_THREAD = _FlushThread()
        _FLUSH_THREAD.start()


def stop_thread():
    """Signal the flush thread to stop and wait until it does."""

    with _FLUSH_THREAD_LOCK:
        if _FLUSH_THREAD is None:
            raise ValueError("FlushThread hasn't been started")

    _FLUSH_THREAD.signal_shutdown()
    # Wait for 1min instead of _FLUSH_THREAD.await_shutdown() because we can
    # sometimes wait indefinitely for a response, causing a task timeout.
    _FLUSH_THREAD.join(60)

    success = not _FLUSH_THREAD.is_alive()
    return success


def flush_after(handler, delay):
    """Add 'handler' to the queue so that it is flushed after 'delay' seconds by the flush thread.

    Return the scheduled event which may be used for later cancellation (see cancel()).
    """

    if not isinstance(handler, logging.Handler):
        raise TypeError("handler must be a logging.Handler instance")

    return _FLUSH_THREAD.submit(handler.flush, delay)


def close_later(handler):
    """Add 'handler' to the queue so that it is closed later by the flush thread.

    Return the scheduled event which may be used for later cancelation (see cancel()).
    """

    if not isinstance(handler, logging.Handler):
        raise TypeError("handler must be a logging.Handler instance")

    # Schedule the event to run immediately. It is possible for the scheduler to not immediately run
    # handler.close() if it has fallen behind as a result of other events taking longer to run than
    # the time available before the next event.
    no_delay = 0.0
    return _FLUSH_THREAD.submit(handler.close, no_delay)


def cancel(event):
    """Attempt to cancel the specified event.

    Returns true if the event was successfully canceled, and returns false otherwise.
    """
    return _FLUSH_THREAD.cancel_event(event)


class _FlushThread(threading.Thread):
    """Asynchronously flush and close logging handlers."""

    _TIMEOUT = 24 * 60 * 60  # =1 day (a long time to have tests run)

    def __init__(self):
        """Initialize the flush thread."""

        threading.Thread.__init__(self, name="FlushThread")
        # Do not wait to flush the logs if interrupted by the user.
        self.daemon = True

        def interruptible_sleep(secs):
            """Wait up to 'secs' seconds or for the 'self.__schedule_updated' event to be set."""

            # Setting 'self.__schedule_updated' in submit() will cause the scheduler to return early
            # from its 'delayfunc'. This makes it so that if a new event is scheduled with
            # delay=0.0, then it will be performed immediately.
            self.__schedule_updated.wait(secs)
            self.__schedule_updated.clear()

        self.__scheduler = sched.scheduler(time.monotonic, interruptible_sleep)
        self.__schedule_updated = threading.Event()
        self.__should_stop = threading.Event()
        self.__terminated = threading.Event()

    def run(self):
        """Continuously flush and close logging handlers."""

        try:
            while not (self.__should_stop.is_set() and self.__scheduler.empty()):
                self.__scheduler.run()

                # Reset 'self.__schedule_updated' here since we've processed all the events
                # thought to exist. Either the queue won't be empty or 'self.__schedule_updated'
                # will get set again later.
                self.__schedule_updated.clear()

                if self.__should_stop.is_set():
                    # If the main thread has asked the flush thread to stop, then either run
                    # whatever has been added to the queue since the scheduler last ran, or exit.
                    continue

                # Otherwise, wait for a new event to be scheduled.
                if self.__scheduler.empty():
                    self.__schedule_updated.wait()
        finally:
            self.__terminated.set()

    def signal_shutdown(self):
        """Indicate to the flush thread that it should exit.

        This will happen once its current queue of logging handlers are flushed and closed.
        """

        self.__should_stop.set()

        # Signal the flush thread to wake up as though there is more work for it to do since we're
        # trying to get it to exit.
        self.__schedule_updated.set()

    def await_shutdown(self):
        """Wait for the flush thread to finish processing its current queue of logging handlers."""

        while not self.__terminated.is_set():
            # Need to pass a timeout to wait() so that KeyboardInterrupt exceptions are propagated.
            self.__terminated.wait(_FlushThread._TIMEOUT)

    def submit(self, action, delay):
        """Schedule 'action' for 'delay' seconds from now.

        Return the scheduled event which may be used for later cancelation (see cancel_event()).
        """

        event = self.__scheduler.enter(delay, 0, action, ())
        self.__schedule_updated.set()
        return event

    def cancel_event(self, event):
        """Attempt to cancel the specified event.

        Return true if the event was successfully canceled, and returns false otherwise.
        """

        try:
            self.__scheduler.cancel(event)
            return True
        except ValueError:
            # We may have failed to cancel the event due to it already being in progress.
            pass
        return False
