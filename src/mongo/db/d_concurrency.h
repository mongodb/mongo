// @file d_concurrency.h
// only used by mongod, thus the name ('d')
// (also used by dbtests test binary, which is running mongod test code)

#pragma once

#include "../util/concurrency/rwlock.h"
#include "db/mongomutex.h"

namespace mongo {

    struct LockState;

    /** a mutex, but reported in curop() - thus a "high level" (HL) one
        some overhead so we don't use this for everything.  the externalobjsort mutex
        uses this, as it can be held for eons. implementation still needed. */
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
            void lockTop(LockState&);
            void lockLocal();
            void lock(const string& db);
            bool locked_r;
            SimpleRWLock *weLocked;
            unsigned *ourCounter;
        public:
            DBRead(const StringData& dbOrNs);
            ~DBRead();
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

    struct LockState {
        LockState() : threadState(0), recursive(0), local(0), other(0), otherLock(0) { }

        // global lock related
        char threadState;             // 0, 'r', 'w', 'R', 'W'
        unsigned recursive;           // nested locking is allowed

        // db lock related
        unsigned local;               // recursive lock count on local db
        unsigned other;
        string otherName;             // which database are we locking and working with (besides local)
        SimpleRWLock *otherLock;      // so we don't have to check the map too often (the map has a mutex)
    };

}
