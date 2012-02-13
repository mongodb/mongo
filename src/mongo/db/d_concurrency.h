// @file d_concurrency.h
// only used by mongod, thus the name ('d')
// (also used by dbtests test binary, which is running mongod test code)

#pragma once

#include "../util/concurrency/rwlock.h"
#include "db/mongomutex.h"

namespace mongo {

    struct LockState;

    class Lock : boost::noncopyable { 
    public:
        static int isLocked();      // true if *anything* is locked (by us)
        static int isReadLocked();  // r or R
        static int somethingWriteLocked(); // w or W
        static bool isW();          // W
        static bool isR();          
        static bool isRW();         // R or W. i.e., we are write-exclusive          
        static bool nested();
        static bool isWriteLocked(const StringData& ns);
        static bool atLeastReadLocked(const StringData& ns); // true if this db is locked
        static void assertAtLeastReadLocked(const StringData& ns);
        static void assertWriteLocked(const StringData& ns);

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
            void lockTop(LockState&);
            void lockLocal();
            void lock(const string& db);
            bool locked_w;
            SimpleRWLock *weLocked;
            int *ourCounter;
        public:
            DBWrite(const StringData& dbOrNs);
            ~DBWrite();
        };
        // lock this database for reading. do not shared_lock globally first, that is handledin herein. 
        class DBRead : boost::noncopyable {
            void lockTop(LockState&);
            void lockLocal();
            void lock(const string& db);
            bool locked_r;
            SimpleRWLock *weLocked;
            int *ourCounter;
        public:
            DBRead(const StringData& dbOrNs);
            ~DBRead();
        };

        // specialty things:
        struct ThreadSpanningOp { 
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

    // writelock is an old helper the code has used for a long time.
    // it now DBWrite locks if ns parm is specified. otherwise global W locks
    class writelock {
        scoped_ptr<Lock::GlobalWrite> lk1;
        scoped_ptr<Lock::DBWrite> lk2;
    public:
        writelock(const string& ns);
        writelock();
    };

    /** parameterized choice of read or write locking */
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

    /** a mutex, but reported in curop() - thus a "high level" (HL) one
        some overhead so we don't use this for everything.  the externalobjsort mutex
        uses this, as it can be held for eons. implementation still needed. */
    class HLMutex : public SimpleMutex {
    public:
        HLMutex(const char *name);
    };

    // implementation stuff
    struct LockState {
        LockState() : threadState(0), recursive(0), local(0), other(0), otherLock(0) { }
        void dump();

        // global lock related
        char threadState;             // 0, 'r', 'w', 'R', 'W'
        unsigned recursive;           // nested locking is allowed

        // db level locking related
        int local;                    // recursive lock count on local db and other db
        int other;                    //   >0 means write lock, <0 read lock
        string otherName;             // which database are we locking and working with (besides local)
        SimpleRWLock *otherLock;      // so we don't have to check the map too often (the map has a mutex)
    };

}
