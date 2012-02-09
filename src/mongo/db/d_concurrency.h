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
        static int isLocked();      // true if *anything* is locked (by us)
        static int isReadLocked();  // r or R
        static int isWriteLocked(); // w or W
        static bool isW();          // W
        static bool isR();          
        static bool nested();

        struct TempRelease {
            TempRelease(); 
            ~TempRelease();
            const bool cant; // true if couldn't because of recursive locking
        private:
            const char type;
        };
        class GlobalWrite : boost::noncopyable { // recursive is ok
            const bool stopGreed;
        public:
            GlobalWrite(bool stopGreed = false); 
            ~GlobalWrite();
            void downgrade(); // W -> R
            void upgrade();   // caution see notes
        };
        struct GlobalRead : boost::noncopyable { // recursive is ok
            GlobalRead(); 
            ~GlobalRead();
        };
        // lock this database. do not shared_lock globally first, that is handledin herein. 
        class DBWrite : boost::noncopyable {
        public:
            DBWrite(const StringData& dbOrNs);
            ~DBWrite();
            // TEMP:
            GlobalWrite w;
        };
        // lock this database for reading. do not shared_lock globally first, that is handledin herein. 
        class DBRead : boost::noncopyable {
        public:
            DBRead(const StringData& dbOrNs);
            ~DBRead();
            // TEMP: 
            GlobalRead r;
        };

        // specialty things:
        /*struct Nongreedy : boost::noncopyable { // temporarily disable greediness of W lock acquisitions
            Nongreedy(); ~Nongreedy();
        };*/
        struct ThreadSpan { 
            static void setWLockedNongreedy();
            static void W_to_R();
            static void unsetW(); // reverts to greedy
            static void unsetR(); // reverts to greedy
        };

    };

    // the below are for backward compatibility.  use Lock classes above instead.

    class readlock {
        scoped_ptr<Lock::GlobalRead> lk1;
        scoped_ptr<Lock::DBRead> lk2;
    public:
        readlock(const string& ns);
        readlock();
    };

    class writelock {
        scoped_ptr<Lock::GlobalWrite> lk1;
        scoped_ptr<Lock::DBWrite> lk2;
    public:
        writelock(const string& ns);
        writelock();
    };

    /* parameterized choice of read or write locking
    */
    class mongolock {
        scoped_ptr<readlock> r;
        scoped_ptr<writelock> w;
    public:
        mongolock(bool write) {
            if( write ) {
                w.reset( new writelock() );
            }
            else {
                r.reset( new readlock() );
            }
        }
    };

}
