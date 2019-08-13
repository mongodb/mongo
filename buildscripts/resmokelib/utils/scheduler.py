"""Version of sched.scheduler with a fixed cancel() method."""

import heapq
import sched


class Scheduler(sched.scheduler):
    """A thread-safe, general purpose event scheduler."""

    def cancel(self, event):
        """Remove an event from the queue.

        Raises a ValueError if the event is not in the queue.
        """

        # The changes from https://hg.python.org/cpython/rev/d8802b055474 made it so sched.Event
        # instances returned by sched.scheduler.enter() and sched.scheduler.enterabs() are treated
        # as equal if they have the same (time, priority). It is therefore possible to remove the
        # wrong event from the list when sched.scheduler.cancel() is called. Note that this is still
        # true even with time.monotonic being the default timefunc, as GetTickCount64() on Windows
        # only has a resolution of ~15ms. We therefore use the `is` operator to remove the correct
        # event from the list.
        with self._lock:
            for i in range(len(self._queue)):
                if self._queue[i] is event:
                    del self._queue[i]
                    heapq.heapify(self._queue)
                    return

            raise ValueError("event not in list")
