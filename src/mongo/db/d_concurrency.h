// @file d_concurrency.h
// only used by mongod, thus the name ('d')
// (also used by dbtests test binary, which is running mongod test code)

#pragma once

namespace mongo {

    class SimpleRWLock;
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

        class ScopedLock;
        // avoid TempRelease when possible.
        struct TempRelease {
            TempRelease(); 
            ~TempRelease();
            const bool cant; // true if couldn't because of recursive locking
            ScopedLock *scopedLk;
        };

        class ScopedLock : boost::noncopyable {
        protected: 
            friend struct TempRelease;
            ScopedLock(); 
            virtual ~ScopedLock();
            virtual void tempRelease() = 0;
            virtual void relock() = 0;
        };

        // note that for these classes recursive locking is ok if the recursive locking "makes sense"
        // i.e. you could grab globalread after globalwrite.

        class GlobalWrite : private ScopedLock {
            const bool stoppedGreed;
            bool noop;
        protected:
            void tempRelease();
            void relock();
        public:
            /** @param stopGreed after acquisition stop greediness of other threads for write locks. this 
                should generally not be used it is for exceptional circumstances. journaling uses it. 
                perhaps this should go away it makes the software more complicated.
            */
            GlobalWrite(bool stopGreed = false); 
            virtual ~GlobalWrite();
            void downgrade(); // W -> R
            bool upgrade();   // caution see notes
        };
        struct GlobalRead : private ScopedLock { // recursive is ok
            bool noop;
        protected:
            void tempRelease();
            void relock();
        public:
            GlobalRead(); 
            virtual ~GlobalRead();
        };
        // lock this database. do not shared_lock globally first, that is handledin herein. 
        class DBWrite : private ScopedLock {
            bool isW(LockState&) const;
            void lockTop(LockState&);
            void lockLocal();
            void lock(const string& db);
            bool locked_w;
            SimpleRWLock *weLocked;
            int *ourCounter;
            const string what;
            void lockDB(const string& ns);
            void unlockDB();
        protected:
            void tempRelease();
            void relock();
        public:
            DBWrite(const StringData& dbOrNs);
            virtual ~DBWrite();
        };
        // lock this database for reading. do not shared_lock globally first, that is handledin herein. 
        class DBRead : private ScopedLock {
            bool isRW(LockState&) const;
            void lockTop(LockState&);
            void lockLocal();
            void lock(const string& db);
            bool locked_r;
            SimpleRWLock *weLocked;
            int *ourCounter;
            string what;
            void lockDB(const string& ns);
            void unlockDB();
        protected:
            void tempRelease();
            void relock();
        public:
            DBRead(const StringData& dbOrNs);
            virtual ~DBRead();
        };

        // specialty things:
        struct ThreadSpanningOp { 
            static void setWLockedNongreedy();
            static void W_to_R();
            static void unsetW(); // reverts to greedy
            static void unsetR(); // reverts to greedy
            static void handoffR(); // doesn't unlock, but changes my thread state back to ''
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
        LockState();
        void dump();
        static void Dump();

        unsigned recursive;           // nested locking is allowed

        // global lock related
        char threadState;             // 0, 'r', 'w', 'R', 'W'

        // db level locking related
        int local;                    // recursive lock count on local db and other db
        int other;                    //   >0 means write lock, <0 read lock
        string otherName;             // which database are we locking and working with (besides local)
        SimpleRWLock *otherLock;      // so we don't have to check the map too often (the map has a mutex)

        // for temprelease
        Lock::ScopedLock *scopedLk;   // for the nonrecursive case. otherwise there would be many
    };

}
