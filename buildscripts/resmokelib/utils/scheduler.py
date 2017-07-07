"""
A thread-safe version of sched.scheduler since the class wasn't made
thread-safe until Python 3.3.
"""

from __future__ import absolute_import

import heapq
import sched
import threading


class Scheduler(sched.scheduler):
    """
    A thread-safe, general purpose event scheduler.
    """

    def __init__(self, timefunc, delayfunc):
        sched.scheduler.__init__(self, timefunc, delayfunc)

        # We use a recursive lock because sched.scheduler.enter() calls sched.scheduler.enterabs().
        self._queue_lock = threading.RLock()

    def enterabs(self, time, priority, action, argument):
        with self._queue_lock:
            return sched.scheduler.enterabs(self, time, priority, action, argument)

    def enter(self, delay, priority, action, argument):
        with self._queue_lock:
            return sched.scheduler.enter(self, delay, priority, action, argument)

    def cancel(self, event):
        with self._queue_lock:
            return sched.scheduler.cancel(self, event)

    def empty(self):
        with self._queue_lock:
            return sched.scheduler.empty(self)

    # The implementation for the run() method was adapted from sched.scheduler.run() in Python 3.6.
    def run(self):
        while True:
            with self._queue_lock:
                if not self._queue:
                    break

                now = self.timefunc()
                event = self._queue[0]

                should_execute = event.time <= now
                if should_execute:
                    heapq.heappop(self._queue)

            if should_execute:
                event.action(*event.argument)

                # sched.scheduler calls delayfunc(0) in order to yield the CPU and let other threads
                # run, so we do the same here.
                self.delayfunc(0)
            else:
                self.delayfunc(event.time - now)

    @property
    def queue(self):
        with self._queue_lock:
            return sched.scheduler.queue.fget(self)
