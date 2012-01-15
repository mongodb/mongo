// @file d_concurrency.h

#pragma once

#include "../util/concurrency/rwlock.h"
#include "db/mongomutex.h"

namespace mongo {

    class ExcludeAllWrites : boost::noncopyable {
        RWLockRecursive::Exclusive lk;
        readlock gslk;
    public:
        ExcludeAllWrites();
    };

    class LockCollectionForReading : boost::noncopyable { 
        readlock lk;
    public:
        explicit LockCollectionForReading(const string& ns) { }
    };

}
