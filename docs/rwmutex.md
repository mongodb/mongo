# MongoDB Reader-Writer Mutex Types

The following are specialized in-house shared mutex types that allow exploiting use-case specific
concurrency semantics to provide low overhead synchronization. Make sure to adopt these primitives
only if your use-case exactly matches the requirements listed below, or consult with the Server
Programmability team.

## WriteRarelyRWMutex

A reader-writer mutex type that assumes frequent reads and almost no writes. Writers can exclusively
lock this mutex type. However, that is considered a very rare exception. Under the hood, it is very
similar to a hazard pointer, where:

- There are per-thread lists that record shared lock acquisitions.
- A writer will go through these per-thread lists and block until the mutex is not referenced by any
  list.

This design allows read locks to be very cheap (i.e. tens of nanoseconds), and linearly scalable
with the number of cores. However, the cost of acquiring a write lock increases with the number of
threads and can be hundreds of microseconds. Therefore, opt for using `WriteRarelyRWMutex` only when
almost all accesses are reads (e.g. replication configuration), and avoid using this mutex type when
writes are not an exception and could happen regularly.

## RWMutex

A reader-writer mutex type that is tailored for frequent reads, and occasional writes. Writers await
completion of active readers, while blocking any new reader. In comparison to `WriteRarelyRWMutex`,
reads are more expensive and less scalable in order to reduce the overhead of occasional writes.
Under the hood, `RWMutex` mimics a counter that records the number of active readers. Writers have
to wait until all readers retire, and block new readers by setting a write intent. This type could
outperform `std::shared_mutex` and `std::mutex` for specific use cases, therefore, prefer using the
alternatives from standard library unless there is a required performance budget to meet, as well as
strong evidence that using `RWMutex` helps with meeting those performance requirements.
