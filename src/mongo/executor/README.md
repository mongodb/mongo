# Executors

Executors are objects used to schedule and execute work asynchronously. Users can schedule tasks on
executors, and the executor will go through scheduled tasks in FIFO order and execute them
asynchronously. The various types of executors provide different properties described below.

## OutOfLineExecutor

The `OutOfLineExecutor` is the base class for other asynchronous execution APIs.
The `OutOfLineExecutor` declares a function `void schedule(Task task)` to delegate asynchronous
execution of `task` to the executor, and each executor type implements `schedule` when extending
`OutOfLineExecutor`. A contract for `OutOfLineExecutor` is that calls to schedule will not block
the caller.
For more details and the semantics of executor APIs, see the comments in the [header file](https://github.com/mongodb/mongo/blob/master/src/mongo/util/out_of_line_executor.h).

### OutOfLineExecutor Wrappers

Each `OutOfLineExecutor` wrapper enforces some property on a provided executor. The wrappers
include:

-   `GuaranteedExecutor`: ensures the scheduled tasks runs exactly once.
-   `GuaranteedExecutorWithFallback`: wraps a preferred and a fallback executor and allows the
    preferred executor to pass tasks to the fallback. The wrapped executors ensure the scheduled tasks
    run exactly once.
-   `CancelableExecutor`: accepts a cancelation token and an executor to add cancelation support to
    the wrapped executor.

## TaskExecutor

`TaskExecutor` is an abstract class inheriting from `OutOfLineExecutor` that supports the notion
of events and callbacks. `TaskExecutor` provides an interface for:

-   Scheduling tasks, with functionality for cancellation or scheduling at a later time if desired.
-   Creating events, having threads subscribe to the events, and notifying the subscribed threads
    when desired.
-   Scheduling remote and exhaust commands from a single or multiple remote hosts.

### Example Usage

-   [Scheduling work and cancellation](https://github.com/mongodb/mongo/blob/311b84df538a5ee9ab4db507f610d8b814bb2099/src/mongo/executor/task_executor_test_common.cpp#L197-L209)
-   [Scheduling remote commands](https://github.com/mongodb/mongo/blob/311b84df538a5ee9ab4db507f610d8b814bb2099/src/mongo/executor/task_executor_test_common.cpp#L568-L586)
-   [Using `scheduleWorkAt`](https://github.com/mongodb/mongo/blob/311b84df538a5ee9ab4db507f610d8b814bb2099/src/mongo/executor/task_executor_test_common.cpp#L532-L566)
-   [Event waiting and signaling](https://github.com/mongodb/mongo/blob/311b84df538a5ee9ab4db507f610d8b814bb2099/src/mongo/executor/task_executor_test_common.cpp#L378-L401)
-   [Using `sleepUntil`](https://github.com/mongodb/mongo/blob/311b84df538a5ee9ab4db507f610d8b814bb2099/src/mongo/executor/task_executor_test_common.cpp#L509-L530)

### Task Executor Variants

-   `ThreadPoolTaskExecutor`: implements the `TaskExecutor` interface and uses a thread pool to
    execute any work scheduled on the executor.
-   `ScopedTaskExecutor`: wraps a `TaskExecutor` and cancels any outstanding operations on
    destruction.
-   `PinnedConnectionTaskExecutor`: wraps a `TaskExecutor` and acts as a `ScopedTaskExecutor` that
    additionally runs all RPCs/remote operations scheduled through it over the same transport connection.
-   `TaskExecutorCursor`: manages a remote cursor that uses an asynchronous task executor to run all
    stages of the command cursor protocol (initial command, getMore, killCursors). Offers a `pinConnections`
    option that utilizes a `PinnedConnectionTaskExecutor` to run all operations on the cursor over the
    same transport connection.
-   `TaskExecutorPool`: represents a pool of `TaskExecutors`. Work which requires a `TaskExecutor` can
    ask for an executor from the pool. This allows for work to be distributed across several executors.
