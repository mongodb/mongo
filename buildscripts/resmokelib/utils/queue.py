"""Extension to the Queue.Queue class.

Added support for the join() method to take a timeout. This is necessary
in order for KeyboardInterrupt exceptions to get propagated.

See https://bugs.python.org/issue1167930 for more details.
"""

import queue as _queue
import time

# Exception that is raised when get_nowait() is called on an empty Queue.
Empty = _queue.Empty  # pylint: disable=invalid-name


class Queue(_queue.Queue):
    """A multi-producer, multi-consumer queue."""

    def join(self, timeout=None):  # pylint: disable=arguments-differ
        """Wait until all items in the queue have been processed or 'timeout' seconds have passed.

        The count of unfinished tasks is incremented whenever an item is added
        to the queue. The count is decremented whenever task_done() is called
        to indicate that all work on the retrieved item was completed.

        When the number of unfinished tasks reaches zero, True is returned.
        If the number of unfinished tasks remains nonzero after 'timeout'
        seconds have passed, then False is returned.
        """
        with self.all_tasks_done:
            if timeout is None:
                while self.unfinished_tasks:
                    self.all_tasks_done.wait()
            elif timeout < 0:
                raise ValueError("timeout must be a nonnegative number")
            else:
                # Pass timeout down to lock acquisition
                deadline = time.time() + timeout
                while self.unfinished_tasks:
                    remaining = deadline - time.time()
                    if remaining <= 0.0:
                        return False
                    self.all_tasks_done.wait(remaining)
        return True
