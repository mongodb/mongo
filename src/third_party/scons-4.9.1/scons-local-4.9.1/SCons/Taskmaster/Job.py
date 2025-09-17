# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

"""Serial and Parallel classes to execute build tasks.

The Jobs class provides a higher level interface to start,
stop, and wait on jobs.
"""

import SCons.compat

import logging
import os
import queue
import signal
import sys
import threading

from enum import Enum

import SCons.Errors
import SCons.Warnings


# The default stack size (in kilobytes) of the threads used to execute
# jobs in parallel.
#
# We use a stack size of 256 kilobytes. The default on some platforms
# is too large and prevents us from creating enough threads to fully
# parallelized the build. For example, the default stack size on linux
# is 8 MBytes.

explicit_stack_size = None
default_stack_size = 256

interrupt_msg = 'Build interrupted.'

class InterruptState:
    def __init__(self) -> None:
        self.interrupted = False

    def set(self) -> None:
        self.interrupted = True

    def __call__(self):
        return self.interrupted


class Jobs:
    """An instance of this class initializes N jobs, and provides
    methods for starting, stopping, and waiting on all N jobs.
    """

    def __init__(self, num, taskmaster) -> None:
        """
        Create 'num' jobs using the given taskmaster. The exact implementation
        used varies with the number of jobs requested and the state of the `legacy_sched` flag
        to `--experimental`.
        """

        # Importing GetOption here instead of at top of file to avoid
        # circular imports
        # pylint: disable=import-outside-toplevel
        from SCons.Script import GetOption

        stack_size = explicit_stack_size
        if stack_size is None:
            stack_size = default_stack_size

        experimental_option = GetOption('experimental') or []
        if 'legacy_sched' in experimental_option:
            if num > 1:
                self.job = LegacyParallel(taskmaster, num, stack_size)
            else:
                self.job = Serial(taskmaster)
        else:
            self.job = NewParallel(taskmaster, num, stack_size)

        self.num_jobs = num

    def run(self, postfunc=lambda: None) -> None:
        """Run the jobs.

        postfunc() will be invoked after the jobs has run. It will be
        invoked even if the jobs are interrupted by a keyboard
        interrupt (well, in fact by a signal such as either SIGINT,
        SIGTERM or SIGHUP). The execution of postfunc() is protected
        against keyboard interrupts and is guaranteed to run to
        completion."""
        self._setup_sig_handler()
        try:
            self.job.start()
        finally:
            postfunc()
            self._reset_sig_handler()

    def were_interrupted(self):
        """Returns whether the jobs were interrupted by a signal."""
        return self.job.interrupted()

    def _setup_sig_handler(self) -> None:
        """Setup an interrupt handler so that SCons can shutdown cleanly in
        various conditions:

          a) SIGINT: Keyboard interrupt
          b) SIGTERM: kill or system shutdown
          c) SIGHUP: Controlling shell exiting

        We handle all of these cases by stopping the taskmaster. It
        turns out that it's very difficult to stop the build process
        by throwing asynchronously an exception such as
        KeyboardInterrupt. For example, the python Condition
        variables (threading.Condition) and queues do not seem to be
        asynchronous-exception-safe. It would require adding a whole
        bunch of try/finally block and except KeyboardInterrupt all
        over the place.

        Note also that we have to be careful to handle the case when
        SCons forks before executing another process. In that case, we
        want the child to exit immediately.
        """
        def handler(signum, stack, self=self, parentpid=os.getpid()) -> None:
            if os.getpid() == parentpid:
                self.job.taskmaster.stop()
                self.job.interrupted.set()
            else:
                os._exit(2)  # pylint: disable=protected-access

        self.old_sigint = signal.signal(signal.SIGINT, handler)
        self.old_sigterm = signal.signal(signal.SIGTERM, handler)
        try:
            self.old_sighup = signal.signal(signal.SIGHUP, handler)
        except AttributeError:
            pass
        if (self.old_sigint is None) or (self.old_sigterm is None) or \
                (hasattr(self, "old_sighup") and self.old_sighup is None):
            msg = "Overwritting previous signal handler which was not installed from Python. " + \
                "Will not be able to reinstate and so will return to default handler."
            SCons.Warnings.warn(SCons.Warnings.SConsWarning, msg)

    def _reset_sig_handler(self) -> None:
        """Restore the signal handlers to their previous state (before the
         call to _setup_sig_handler()."""
        sigint_to_use = self.old_sigint if self.old_sigint is not None else signal.SIG_DFL
        sigterm_to_use = self.old_sigterm if self.old_sigterm is not None else signal.SIG_DFL
        signal.signal(signal.SIGINT, sigint_to_use)
        signal.signal(signal.SIGTERM, sigterm_to_use)
        try:
            sigterm_to_use = self.old_sighup if self.old_sighup is not None else signal.SIG_DFL
            signal.signal(signal.SIGHUP, sigterm_to_use)
        except AttributeError:
            pass


class Serial:
    """This class is used to execute tasks in series, and is more efficient
    than Parallel, but is only appropriate for non-parallel builds. Only
    one instance of this class should be in existence at a time.

    This class is not thread safe.
    """

    def __init__(self, taskmaster) -> None:
        """Create a new serial job given a taskmaster.

        The taskmaster's next_task() method should return the next task
        that needs to be executed, or None if there are no more tasks. The
        taskmaster's executed() method will be called for each task when it
        is successfully executed, or failed() will be called if it failed to
        execute (e.g. execute() raised an exception)."""

        self.taskmaster = taskmaster
        self.interrupted = InterruptState()

    def start(self):
        """Start the job. This will begin pulling tasks from the taskmaster
        and executing them, and return when there are no more tasks. If a task
        fails to execute (i.e. execute() raises an exception), then the job will
        stop."""

        while True:
            task = self.taskmaster.next_task()

            if task is None:
                break

            try:
                task.prepare()
                if task.needs_execute():
                    task.execute()
            except Exception:
                if self.interrupted():
                    try:
                        raise SCons.Errors.BuildError(
                            task.targets[0], errstr=interrupt_msg)
                    except Exception:
                        task.exception_set()
                else:
                    task.exception_set()

                # Let the failed() callback function arrange for the
                # build to stop if that's appropriate.
                task.failed()
            else:
                task.executed()

            task.postprocess()
        self.taskmaster.cleanup()


class Worker(threading.Thread):
    """A worker thread waits on a task to be posted to its request queue,
    dequeues the task, executes it, and posts a tuple including the task
    and a boolean indicating whether the task executed successfully. """

    def __init__(self, requestQueue, resultsQueue, interrupted) -> None:
        super().__init__()
        self.daemon = True
        self.requestQueue = requestQueue
        self.resultsQueue = resultsQueue
        self.interrupted = interrupted
        self.start()

    def run(self):
        while True:
            task = self.requestQueue.get()

            if task is None:
                # The "None" value is used as a sentinel by
                # ThreadPool.cleanup().  This indicates that there
                # are no more tasks, so we should quit.
                break

            try:
                if self.interrupted():
                    raise SCons.Errors.BuildError(
                        task.targets[0], errstr=interrupt_msg)
                task.execute()
            except Exception:
                task.exception_set()
                ok = False
            else:
                ok = True

            self.resultsQueue.put((task, ok))

class ThreadPool:
    """This class is responsible for spawning and managing worker threads."""

    def __init__(self, num, stack_size, interrupted) -> None:
        """Create the request and reply queues, and 'num' worker threads.

        One must specify the stack size of the worker threads. The
        stack size is specified in kilobytes.
        """
        self.requestQueue = queue.Queue(0)
        self.resultsQueue = queue.Queue(0)

        try:
            prev_size = threading.stack_size(stack_size * 1024)
        except RuntimeError as e:
            # Only print a warning if the stack size has been explicitly set.
            if explicit_stack_size is not None:
                msg = "Setting stack size is unsupported by this version of Python:\n    " + \
                    e.args[0]
                SCons.Warnings.warn(SCons.Warnings.StackSizeWarning, msg)
        except ValueError as e:
            msg = "Setting stack size failed:\n    " + str(e)
            SCons.Warnings.warn(SCons.Warnings.StackSizeWarning, msg)

        # Create worker threads
        self.workers = []
        for _ in range(num):
            worker = Worker(self.requestQueue, self.resultsQueue, interrupted)
            self.workers.append(worker)

        if 'prev_size' in locals():
            threading.stack_size(prev_size)

    def put(self, task) -> None:
        """Put task into request queue."""
        self.requestQueue.put(task)

    def get(self):
        """Remove and return a result tuple from the results queue."""
        return self.resultsQueue.get()

    def preparation_failed(self, task) -> None:
        self.resultsQueue.put((task, False))

    def cleanup(self) -> None:
        """
        Shuts down the thread pool, giving each worker thread a
        chance to shut down gracefully.
        """
        # For each worker thread, put a sentinel "None" value
        # on the requestQueue (indicating that there's no work
        # to be done) so that each worker thread will get one and
        # terminate gracefully.
        for _ in self.workers:
            self.requestQueue.put(None)

        # Wait for all of the workers to terminate.
        #
        # If we don't do this, later Python versions (2.4, 2.5) often
        # seem to raise exceptions during shutdown.  This happens
        # in requestQueue.get(), as an assertion failure that
        # requestQueue.not_full is notified while not acquired,
        # seemingly because the main thread has shut down (or is
        # in the process of doing so) while the workers are still
        # trying to pull sentinels off the requestQueue.
        #
        # Normally these terminations should happen fairly quickly,
        # but we'll stick a one-second timeout on here just in case
        # someone gets hung.
        for worker in self.workers:
            worker.join(1.0)
        self.workers = []

class LegacyParallel:
    """This class is used to execute tasks in parallel, and is somewhat
    less efficient than Serial, but is appropriate for parallel builds.

    This class is thread safe.
    """

    def __init__(self, taskmaster, num, stack_size) -> None:
        """Create a new parallel job given a taskmaster.

        The taskmaster's next_task() method should return the next
        task that needs to be executed, or None if there are no more
        tasks. The taskmaster's executed() method will be called
        for each task when it is successfully executed, or failed()
        will be called if the task failed to execute (i.e. execute()
        raised an exception).

        Note: calls to taskmaster are serialized, but calls to
        execute() on distinct tasks are not serialized, because
        that is the whole point of parallel jobs: they can execute
        multiple tasks simultaneously. """

        self.taskmaster = taskmaster
        self.interrupted = InterruptState()
        self.tp = ThreadPool(num, stack_size, self.interrupted)

        self.maxjobs = num

    def start(self):
        """Start the job. This will begin pulling tasks from the
        taskmaster and executing them, and return when there are no
        more tasks. If a task fails to execute (i.e. execute() raises
        an exception), then the job will stop."""

        jobs = 0

        while True:
            # Start up as many available tasks as we're
            # allowed to.
            while jobs < self.maxjobs:
                task = self.taskmaster.next_task()
                if task is None:
                    break

                try:
                    # prepare task for execution
                    task.prepare()
                except Exception:
                    task.exception_set()
                    task.failed()
                    task.postprocess()
                else:
                    if task.needs_execute():
                        # dispatch task
                        self.tp.put(task)
                        jobs += 1
                    else:
                        task.executed()
                        task.postprocess()

            if not task and not jobs:
                break

            # Let any/all completed tasks finish up before we go
            # back and put the next batch of tasks on the queue.
            while True:
                task, ok = self.tp.get()
                jobs -= 1

                if ok:
                    task.executed()
                else:
                    if self.interrupted():
                        try:
                            raise SCons.Errors.BuildError(
                                task.targets[0], errstr=interrupt_msg)
                        except Exception:
                            task.exception_set()

                    # Let the failed() callback function arrange
                    # for the build to stop if that's appropriate.
                    task.failed()

                task.postprocess()

                if self.tp.resultsQueue.empty():
                    break

        self.tp.cleanup()
        self.taskmaster.cleanup()

# An experimental new parallel scheduler that uses a leaders/followers pattern.
class NewParallel:

    class State(Enum):
        READY = 0
        SEARCHING = 1
        STALLED = 2
        COMPLETED = 3

    class Worker(threading.Thread):
        def __init__(self, owner) -> None:
            super().__init__()
            self.daemon = True
            self.owner = owner
            self.start()

        def run(self) -> None:
            self.owner._work()

    class FakeLock(object):
        def lock(self):
            pass
        def unlock(self):
            pass
        def __enter__(self):
            pass
        def __exit__(self, *args):
            pass

    class FakeCondition(object):
        def __init__(self, lock):
            pass
        def wait(self):
            fatal();
        def notify(self):
            pass
        def notify_all(self):
            pass
        def __enter__(self):
            pass
        def __exit__(self, *args):
            pass

    def __init__(self, taskmaster, num, stack_size) -> None:
        self.taskmaster = taskmaster
        self.max_workers = num
        self.stack_size = stack_size
        self.interrupted = InterruptState()
        self.workers = []

        # The `tm_lock` is what ensures that we only have one
        # thread interacting with the taskmaster at a time. It
        # also protects access to our state that gets updated
        # concurrently. The `can_search_cv` is associated with
        # this mutex.
        self.tm_lock = (threading.Lock if self.max_workers > 1 else NewParallel.FakeLock)()

        # Guarded under `tm_lock`.
        self.jobs = 0
        self.state = NewParallel.State.READY

        # The `can_search_cv` is used to manage a leader /
        # follower pattern for access to the taskmaster, and to
        # awaken from stalls.
        self.can_search_cv = (threading.Condition if self.max_workers > 1 else NewParallel.FakeCondition)(self.tm_lock)

        # The queue of tasks that have completed execution. The
        # next thread to obtain `tm_lock`` will retire them.
        self.results_queue_lock = (threading.Lock if self.max_workers > 1 else NewParallel.FakeLock)()
        self.results_queue = []

        if self.taskmaster.trace:
            self.trace = self._setup_logging()
        else:
            self.trace = False

    def _setup_logging(self):
        jl = logging.getLogger("Job")
        jl.setLevel(level=logging.DEBUG)
        jl.addHandler(self.taskmaster.trace.log_handler)
        return jl

    def trace_message(self, message) -> None:
        # This grabs the name of the function which calls trace_message()
        method_name = sys._getframe(1).f_code.co_name + "():"
        thread_id=threading.get_ident()
        self.trace.debug('%s.%s [Thread:%s] %s' % (type(self).__name__, method_name, thread_id, message))

    def start(self) -> None:
        if self.max_workers == 1:
            self._work()
        else:
            self._start_worker()
            while len(self.workers) > 0:
                self.workers[0].join()
                self.workers.pop(0)
        self.taskmaster.cleanup()

    def _maybe_start_worker(self) -> None:
        if self.max_workers > 1 and len(self.workers) < self.max_workers:
            if self.jobs >= len(self.workers):
                self._start_worker()

    def _start_worker(self) -> None:
        prev_size = self._adjust_stack_size()
        if self.trace:
            self.trace_message("Starting new worker thread")
        self.workers.append(NewParallel.Worker(self))
        self._restore_stack_size(prev_size)

    def _adjust_stack_size(self):
        try:
            prev_size = threading.stack_size(self.stack_size * 1024)
            return prev_size
        except AttributeError as e:
            # Only print a warning if the stack size has been
            # explicitly set.
            if explicit_stack_size is not None:
                msg = "Setting stack size is unsupported by this version of Python:\n    " + \
                    e.args[0]
                SCons.Warnings.warn(SCons.Warnings.StackSizeWarning, msg)
        except ValueError as e:
            msg = "Setting stack size failed:\n    " + str(e)
            SCons.Warnings.warn(SCons.Warnings.StackSizeWarning, msg)

        return None

    def _restore_stack_size(self, prev_size) -> None:
        if prev_size is not None:
            threading.stack_size(prev_size)

    def _work(self):

        task = None

        while True:

            # Obtain `tm_lock`, granting exclusive access to the taskmaster.
            with self.can_search_cv:

                if self.trace:
                    self.trace_message("Gained exclusive access")

                # Capture whether we got here with `task` set,
                # then drop our reference to the task as we are no
                # longer interested in the actual object.
                completed_task = (task is not None)
                task = None

                # We will only have `completed_task` set here if
                # we have looped back after executing a task. If
                # we have completed a task and find that we are
                # stalled, we should speculatively indicate that
                # we are no longer stalled by transitioning to the
                # 'ready' state which will bypass the condition
                # wait so that we immediately process the results
                # queue and hopefully light up new
                # work. Otherwise, stay stalled, and we will wait
                # in the condvar. Some other thread will come back
                # here with a completed task.
                if self.state == NewParallel.State.STALLED and completed_task:
                    if self.trace:
                        self.trace_message("Detected stall with completed task, bypassing wait")
                    self.state = NewParallel.State.READY

                # Wait until we are neither searching nor stalled.
                while self.state == NewParallel.State.SEARCHING or self.state == NewParallel.State.STALLED:
                    if self.trace:
                        self.trace_message("Search already in progress, waiting")
                    self.can_search_cv.wait()

                # If someone set the completed flag, bail.
                if self.state == NewParallel.State.COMPLETED:
                    if self.trace:
                        self.trace_message("Completion detected, breaking from main loop")
                    break

                # Set the searching flag to indicate that a thread
                # is currently in the critical section for
                # taskmaster work.
                #
                if self.trace:
                    self.trace_message("Starting search")
                self.state = NewParallel.State.SEARCHING

                # Bulk acquire the tasks in the results queue
                # under the result queue lock, then process them
                # all outside that lock. We need to process the
                # tasks in the results queue before looking for
                # new work because we might be unable to find new
                # work if we don't.
                results_queue = []
                with self.results_queue_lock:
                    results_queue, self.results_queue = self.results_queue, results_queue

                if self.trace:
                    self.trace_message(f"Found {len(results_queue)} completed tasks to process")
                for (rtask, rresult) in results_queue:
                    if rresult:
                        rtask.executed()
                    else:
                        if self.interrupted():
                            try:
                                raise SCons.Errors.BuildError(
                                    rtask.targets[0], errstr=interrupt_msg)
                            except Exception:
                                rtask.exception_set()

                        # Let the failed() callback function arrange
                        # for the build to stop if that's appropriate.
                        rtask.failed()

                    rtask.postprocess()
                    self.jobs -= 1

                # We are done with any task objects that were in
                # the results queue.
                results_queue.clear()

                # Now, turn the crank on the taskmaster until we
                # either run out of tasks, or find a task that
                # needs execution. If we run out of tasks, go idle
                # until results arrive if jobs are pending, or
                # mark the walk as complete if not.
                while self.state == NewParallel.State.SEARCHING:
                    if self.trace:
                        self.trace_message("Searching for new tasks")
                    task = self.taskmaster.next_task()

                    if task:
                        # We found a task. Walk it through the
                        # task lifecycle. If it does not need
                        # execution, just complete the task and
                        # look for the next one. Otherwise,
                        # indicate that we are no longer searching
                        # so we can drop out of this loop, execute
                        # the task outside the lock, and allow
                        # another thread in to search.
                        try:
                            task.prepare()
                        except Exception:
                            task.exception_set()
                            task.failed()
                            task.postprocess()
                        else:
                            if not task.needs_execute():
                                if self.trace:
                                    self.trace_message("Found internal task")
                                task.executed()
                                task.postprocess()
                            else:
                                self.jobs += 1
                                if self.trace:
                                    self.trace_message("Found task requiring execution")
                                self.state = NewParallel.State.READY
                                self.can_search_cv.notify()
                                # This thread will be busy taking care of
                                # `execute`ing this task. If we haven't
                                # reached the limit, spawn a new thread to
                                # turn the crank and find the next task.
                                self._maybe_start_worker()

                    else:
                        # We failed to find a task, so this thread
                        # cannot continue turning the taskmaster
                        # crank. We must exit the loop.
                        if self.jobs:
                            # No task was found, but there are
                            # outstanding jobs executing that
                            # might unblock new tasks when they
                            # complete. Transition to the stalled
                            # state. We do not need a notify,
                            # because we know there are threads
                            # outstanding that will re-enter the
                            # loop.
                            #
                            if self.trace:
                                self.trace_message("Found no task requiring execution, but have jobs: marking stalled")
                            self.state = NewParallel.State.STALLED
                        else:
                            # We didn't find a task and there are
                            # no jobs outstanding, so there is
                            # nothing that will ever return
                            # results which might unblock new
                            # tasks. We can conclude that the walk
                            # is complete. Update our state to
                            # note completion and awaken anyone
                            # sleeping on the condvar.
                            #
                            if self.trace:
                                self.trace_message("Found no task requiring execution, and have no jobs: marking complete")
                            self.state = NewParallel.State.COMPLETED
                            self.can_search_cv.notify_all()

            # We no longer hold `tm_lock` here. If we have a task,
            # we can now execute it. If there are threads waiting
            # to search, one of them can now begin turning the
            # taskmaster crank in NewParallel.
            if task:
                if self.trace:
                    self.trace_message("Executing task")
                ok = True
                try:
                    if self.interrupted():
                        raise SCons.Errors.BuildError(
                            task.targets[0], errstr=interrupt_msg)
                    task.execute()
                except Exception:
                    ok = False
                    task.exception_set()

                # Grab the results queue lock and enqueue the
                # executed task and state. The next thread into
                # the searching loop will complete the
                # postprocessing work under the taskmaster lock.
                #
                if self.trace:
                    self.trace_message("Enqueueing executed task results")
                with self.results_queue_lock:
                    self.results_queue.append((task, ok))

            # Tricky state "fallthrough" here. We are going back
            # to the top of the loop, which behaves differently
            # depending on whether `task` is set. Do not perturb
            # the value of the `task` variable if you add new code
            # after this comment.

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
