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
        class GlobalWrite : boost::noncopyable { // stop the world 
        public:
            GlobalWrite(); 
            ~GlobalWrite();
            struct TempRelease {
                TempRelease(); ~TempRelease();
            };
        };
        class GlobalRead : boost::noncopyable { // stop all writers 
        };
        class DBWrite : boost::noncopyable { // exclusive for this db
        public:
            DBWrite(const StringData& dbOrNs);
            ~DBWrite();
        };
        class DBRead : boost::noncopyable { // shared for this db
        public:
            DBRead(const StringData& dbOrNs);
            ~DBRead();
        };
    };

}
