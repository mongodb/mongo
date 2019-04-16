"""Utility code to execute code in parallel."""

import queue
import threading
import time
from multiprocessing import cpu_count
from typing import Any, Callable, List


def parallel_process(items, func):
    # type: (List[Any], Callable[[Any], bool]) -> bool
    """Run a set of work items to completion and wait."""
    try:
        cpus = cpu_count()
    except NotImplementedError:
        cpus = 1

    task_queue = queue.Queue()  # type: queue.Queue

    # Use a list so that worker function will capture this variable
    pp_event = threading.Event()
    pp_result = [True]
    pp_lock = threading.Lock()

    def worker():
        # type: () -> None
        """Worker thread to process work items in parallel."""
        while not pp_event.is_set():
            try:
                item = task_queue.get_nowait()
            except queue.Empty:
                # if the queue is empty, exit the worker thread
                pp_event.set()
                return

            try:
                ret = func(item)
            finally:
                # Tell the queue we finished with the item
                task_queue.task_done()

            # Return early if we fail, and signal we are done
            if not ret:
                with pp_lock:
                    pp_result[0] = False

                pp_event.set()
                return

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
    while not pp_event.wait(1):
        time.sleep(1)

    for thread in threads:
        thread.join()

    return pp_result[0]
