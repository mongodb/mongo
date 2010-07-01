util/concurrency/ files

list.h - a list class that is lock-free for reads
rwlock.h - read/write locks (RWLock)
msg.h - message passing between threads
task.h - an abstraction around threads
mutex.h - small enhancements that wrap boost::mutex
thread_pool.h 
mvar.h
 This is based on haskell's MVar synchronization primitive:
 http://www.haskell.org/ghc/docs/latest/html/libraries/base-4.2.0.0/Control-Concurrent-MVar.html
 It is a thread-safe queue that can hold at most one object.
 You can also think of it as a box that can be either full or empty.
value.h
 Atomic wrapper for values/objects that are copy constructable / assignable
