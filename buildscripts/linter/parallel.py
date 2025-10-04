"""Utility code to execute code in parallel."""

import queue
import threading
import time
from multiprocessing import cpu_count


def parallel_process(items, func):
    # type: (List[Any], Callable[[Any], bool]) -> bool
    """
    Run a set of work items to completion and wait.

    :returns whether all tasks were successful.
    """
    try:
        cpus = cpu_count()
    except NotImplementedError:
        cpus = 1

    task_queue = queue.Queue()  # type: queue.Queue

    has_failure_event = threading.Event()

    def worker():
        # type: () -> None
        """Worker thread to process work items in parallel."""
        while True:
            try:
                item = task_queue.get_nowait()
            except queue.Empty:
                # if the queue is empty, exit the worker thread
                return

            try:
                ret = func(item)
            finally:
                # Tell the queue we finished with the item
                task_queue.task_done()

            if not ret:
                has_failure_event.set()

    # Enqueue all the work we want to process
    for item in items:
        task_queue.put(item)

    # Process all the work
    threads = []
    for _ in range(cpus):
        thread = threading.Thread(target=worker)

        thread.daemon = True
        thread.start()
        threads.append(thread)

    # Wait for the threads to finish
    # Loop with a timeout so that we can process Ctrl-C interrupts
    while not queue.Empty:
        time.sleep(1)

    for thread in threads:
        thread.join()

    return not has_failure_event.is_set()
