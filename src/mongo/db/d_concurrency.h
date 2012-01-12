// @file d_concurrency.h

#pragma once

#include "../util/concurrency/rwlock.h"
#include "db/mongomutex.h"

namespace mongo {

    namespace clcimpl {
        enum LockStates { Unlocked, AcquireShared=1, LockedShared=2, AcquireExclusive=4, LockedExclusive=8 };
        class Shared : boost::noncopyable { 
            unsigned& state;
            RWLock *rw;
        public:
            Shared(unsigned& state, RWLock& lock);
            ~Shared();
            bool recursed() const { return rw == 0; }
        };
        class Exclusive : boost::noncopyable { 
            unsigned& state;
            RWLock *rw;
        public:
            Exclusive(unsigned& state, RWLock& lock);
            ~Exclusive();
        };
    }

    typedef readlock GlobalSharedLock;

    class ExcludeAllWrites : boost::noncopyable {
        clcimpl::Exclusive lk;
        GlobalSharedLock gslk;
    public:
        ExcludeAllWrites();
        ~ExcludeAllWrites();
    };

    class todoGlobalWriteLock : boost::noncopyable { 
    public:
    };

    class LockCollectionForReading : boost::noncopyable { 
        GlobalSharedLock gslk;
        clcimpl::Shared clk;
    public:
        LockCollectionForReading(string coll);
        ~LockCollectionForReading();
    };

#if defined(CLC)
    class LockCollectionForWriting : boost::noncopyable {
        struct Locks { 
            Locks(string ns);
            SimpleRWLock::Shared excluder;
            GlobalSharedLock gslk;
            rwlock clk;
        };
        scoped_ptr<Locks> locks;
    public:
        LockCollectionForWriting(string db);
        ~LockCollectionForWriting();
    };
#else
#endif

}
