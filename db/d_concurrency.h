// @file d_concurrency.h

#pragma once

#include "../util/concurrency/rwlock.h"
#include "db/mongomutex.h"

namespace mongo {

    class LockCollectionForReading { 
        struct Locks { 
            Locks(string ns);
            GlobalSharedLock gslk;
            rwlock_shared clk;
        };
        scoped_ptr<Locks> locks;
    public:
        LockCollectionForReading(string coll);
        ~LockCollectionForReading();
    };

    class LockCollectionForWriting {
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

    class ExcludeAllWrites {
        SimpleRWLock::Exclusive lk;
        GlobalSharedLock gslk;
    public:
        ExcludeAllWrites();
        ~ExcludeAllWrites();
    };

}
