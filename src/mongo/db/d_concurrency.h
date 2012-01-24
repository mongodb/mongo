// @file d_concurrency.h

#pragma once

#include "../util/concurrency/rwlock.h"
#include "db/mongomutex.h"

namespace mongo {

    // a mutex, but reported in curop() - thus a "high level" (HL) one
    // some overhead so we don't use this for everything
    class HLMutex : public SimpleMutex {
    public:
        HLMutex(const char *name);
    };

    class Lock : boost::noncopyable { 
    public:
        static bool isLocked(); // true if *anything* is locked (by us)
        class GlobalWrite : boost::noncopyable {
        public:
            GlobalWrite(); 
            ~GlobalWrite();
            struct TempRelease {
                TempRelease(); ~TempRelease();
            };
        };
        class GlobalRead : boost::noncopyable {
        public:
            GlobalRead(); 
            ~GlobalRead();
        };
        // lock this database. do not shared_lock globally first, that is handledin herein. 
        class DBWrite : boost::noncopyable {
        public:
            DBWrite(const StringData& dbOrNs);
            ~DBWrite();
        };
        // lock this database for reading. do not shared_lock globally first, that is handledin herein. 
        class DBRead : boost::noncopyable {
        public:
            DBRead(const StringData& dbOrNs);
            ~DBRead();
        };
    };

}
