# Thread Pools

A thread pool ([Wikipedia][thread_pools_wikipedia]) accepts and executes
lightweight work items called "tasks", using a carefully managed group
of dedicated long-running worker threads. The worker threads perform
the work items in parallel without forcing each work item to assume the
burden of starting and destroying a dedicated thead.

## Classes

### `ThreadPoolInterface`

The [`ThreadPoolInterface`][thread_pool_interface.h] abstract interface is
an extension of the `OutOfLineExecutor` (see [the executors architecture
guide][executors]) abstract interface, adding `startup`, `shutdown`, and
`join` virtual member functions. It is the base class for our thread
pool classes.

### `ThreadPool`

[`ThreadPool`][thread_pool.h] is the most basic concrete thread pool. The
number of worker threads is adaptive, but configurable with a min/max
range. Idle worker threads are reaped (down to the configured min), while
new worker threads can be created when needed (up to the configured max).

### `ThreadPoolTaskExecutor`

[`ThreadPoolTaskExecutor`][thread_pool_task_executor.h] is not a thread
pool, but rather a `TaskExecutor` that uses a `ThreadPoolInterface` and
a `NetworkInterface` to execute scheduled tasks. It's configured with a
`ThreadPoolInterface` over which it _takes_ ownership, and a
`NetworkInterface`, of which it _shares_ ownership. With these resources
it implements the elaborate `TaskExecutor` interface (see [executors]).

### `NetworkInterfaceThreadPool`

[`NetworkInterfaceThreadPool`][network_interface_thread_pool.h] is a
thread pool implementation that doesn't actually own any worker threads.
It runs its tasks on the background thread of a
[`NetworkInterface`][network_interface.h].

Incoming tasks that are scheduled from the `NetworkInterface`'s thread
are run immediately. Otherwise they are queued to be run by the
`NetworkInterface` thread when it is available.

### `ThreadPoolMock`

[`ThreadPoolMock`][thread_pool_mock.h] is a `ThreadPoolInterface`. It is not
a mock of a `ThreadPool`. It has no configurable stored responses. It has
one worker thread and a pointer to a `NetworkInterfaceMock`, and with these
resources it simulates a thread pool well enough to be used by a
`ThreadPoolTaskExecutor` in unit tests.

[thread_pools_wikipedia]: https://en.wikipedia.org/wiki/Thread_pool
[executors]: ../src/mongo/executor/README.md
[thread_pool_interface.h]: ../src/mongo/util/concurrency/thread_pool_interface.h
[thread_pool.h]: ../src/mongo/util/concurrency/thread_pool.h
[thread_pool_task_executor.h]: ../src/mongo/executor/thread_pool_task_executor.h
[network_interface_thread_pool.h]: ../src/mongo/executor/network_interface_thread_pool.h
[network_interface.h]: ../src/mongo/executor/network_interface.h
[thread_pool_mock.h]: ../src/mongo/executor/thread_pool_mock.h
