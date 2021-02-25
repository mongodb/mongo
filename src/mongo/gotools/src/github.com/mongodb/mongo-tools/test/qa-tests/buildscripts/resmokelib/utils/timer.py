"""
Alternative to the threading.Timer class.

Enables a timer to be restarted without needing to construct a new thread
each time. This is necessary to execute periodic actions, e.g. flushing
log messages to buildlogger, while avoiding errors related to "can't start
new thread" that would otherwise occur on Windows.
"""

from __future__ import absolute_import

import threading


class AlarmClock(threading.Thread):
    """
    Calls a function after a specified number of seconds.
    """

    def __init__(self, interval, func, args=None, kwargs=None):
        """
        Initializes the timer with a function to periodically execute.
        """

        threading.Thread.__init__(self)

        # A non-dismissed timer should not prevent the program from exiting
        self.daemon = True

        self.interval = interval
        self.func = func
        self.args = args if args is not None else []
        self.kwargs = kwargs if kwargs is not None else {}

        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)

        self.snoozed = False  # canceled for one execution
        self.dismissed = False  # canceled for all time
        self.restarted = False

    def dismiss(self):
        """
        Disables the timer.
        """

        with self.lock:
            self.dismissed = True
            self.cond.notify_all()

        self.join()  # Tidy up the started thread.

    cancel = dismiss  # Expose API compatible with that of threading.Timer.

    def snooze(self):
        """
        Skips the next execution of 'func' if it has not already started.
        """

        with self.lock:
            if self.dismissed:
                raise ValueError("Timer cannot be snoozed if it has been dismissed")

            self.snoozed = True
            self.restarted = False
            self.cond.notify_all()

    def reset(self):
        """
        Restarts the timer, causing it to wait 'interval' seconds before calling
        'func' again.
        """

        with self.lock:
            if self.dismissed:
                raise ValueError("Timer cannot be reset if it has been dismissed")

            if not self.snoozed:
                raise ValueError("Timer cannot be reset if it has not been snoozed")

            self.restarted = True
            self.cond.notify_all()

    def run(self):
        """
        Repeatedly calls 'func' with a delay of 'interval' seconds between executions.

        If the timer is snoozed before 'func' is called, then it waits to be reset.
        After it has been reset, the timer will again wait 'interval' seconds and
        then try to call 'func'.

        If the timer is dismissed, then no subsequent executions of 'func' are made.
        """

        while True:
            with self.lock:
                if self.dismissed:
                    return

                # Wait for the specified amount of time.
                self.cond.wait(self.interval)

                if self.dismissed:
                    return

                # If the timer was snoozed, then it should wait to be reset.
                if self.snoozed:
                    while not self.restarted:
                        self.cond.wait()

                        if self.dismissed:
                            return

                    self.restarted = False
                    self.snoozed = False
                    continue

            # Execute the function after the lock has been released to prevent potential deadlocks
            # with the invoked function.
            self.func(*self.args, **self.kwargs)

            # Reacquire the lock.
            with self.lock:
                # Ignore snoozes that took place while the function was being executed.
                self.snoozed = False
