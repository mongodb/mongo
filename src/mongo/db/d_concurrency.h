// @file d_concurrency.h
// only used by mongod, thus the name ('d')
// (also used by dbtests test binary, which is running mongod test code)

#pragma once

#include "mongo/util/concurrency/qlock.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/bson/stringdata.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lockstat.h"

namespace mongo {

    class WrapperForRWLock;
    class LockState;

    class Lock : boost::noncopyable { 
    public:
        enum Nestable { notnestable=0, local, admin };
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

        static bool dbLevelLockingEnabled(); 

        class ScopedLock;

        // note: avoid TempRelease when possible. not a good thing.
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
            virtual void tempRelease() = 0;
            virtual void relock() = 0;
        public:
            virtual ~ScopedLock();
        };

        // note that for these classes recursive locking is ok if the recursive locking "makes sense"
        // i.e. you could grab globalread after globalwrite.

        class GlobalWrite : public ScopedLock {
            bool stoppedGreed;
            bool noop;
        protected:
            void tempRelease();
            void relock();
        public:
            /** @param stopGreed after acquisition stop greediness of other threads for write locks. this 
                should generally not be used it is for exceptional circumstances. journaling uses it. 
                perhaps this should go away it makes the software more complicated.
            */
            // timeoutms is only for writelocktry -- deprecated -- do not use
            GlobalWrite(bool stopGreed = false, int timeoutms = -1 ); 
            virtual ~GlobalWrite();
            void downgrade(); // W -> R
            bool upgrade();   // caution see notes
        };
        class GlobalRead : public ScopedLock { // recursive is ok
        public:
            bool noop;
        protected:
            void tempRelease();
            void relock();
        public:
            // timeoutms is only for readlocktry -- deprecated -- do not use
            GlobalRead( int timeoutms = -1 ); 
            virtual ~GlobalRead();
        };
        // lock this database. do not shared_lock globally first, that is handledin herein. 
        class DBWrite : public ScopedLock {
            bool isW(LockState&) const;
            void lockTop(LockState&);
            void lockNestable(Nestable db);
            void lockOther(const string& db);
            bool locked_w;
            bool locked_W;
            WrapperForRWLock *weLocked;
            const string what;
            bool _nested;
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
        class DBRead : public ScopedLock {
            bool isRW(LockState&) const;
            void lockTop(LockState&);
            void lockNestable(Nestable db);
            void lockOther(const string& db);
            bool locked_r;
            WrapperForRWLock *weLocked;
            string what;
            bool _nested;
            void lockDB(const string& ns);
            void unlockDB();
        protected:
            void tempRelease();
            void relock();
        public:
            DBRead(const StringData& dbOrNs);
            virtual ~DBRead();
        };

    };

    class readlocktry : boost::noncopyable {
        bool _got;
        scoped_ptr<Lock::GlobalRead> _dbrlock;
    public:
        readlocktry( int tryms );
        ~readlocktry();
        bool got() const { return _got; }
    };

    class writelocktry : boost::noncopyable {
        bool _got;
        scoped_ptr<Lock::GlobalWrite> _dbwlock;
    public:
        writelocktry( int tryms );
        ~writelocktry();
        bool got() const { return _got; }
    };

    /** a mutex, but reported in curop() - thus a "high level" (HL) one
        some overhead so we don't use this for everything.  the externalobjsort mutex
        uses this, as it can be held for eons. implementation still needed. */
    class HLMutex : public SimpleMutex {
        LockStat ls;
    public:
        HLMutex(const char *name);
    };

    // implementation stuff
    // per thread
    class LockState {
    public:
        LockState();
        void dump();
        static void Dump();
        void reportState(BSONObjBuilder& b);
        
        unsigned recursiveCount() const { return _recursive; }

        /**
         * @return 0 rwRW
         */
        char threadState() const { return _threadState; }
        
        void locked( char newState ); // RWrw
        void unlocked(); // _threadState = 0
        
        /**
         * you have to be locked already to call this
         * this is mostly for W_to_R or R_to_W
         */
        void changeLockState( char newstate );

        Lock::Nestable whichNestable() const { return _whichNestable; }
        int nestableCount() const { return _nestableCount; }
        
        int otherCount() const { return _otherCount; }
        string otherName() const { return _otherName; }
        WrapperForRWLock* otherLock() const { return _otherLock; }
        
        void enterScopedLock( Lock::ScopedLock* lock );
        Lock::ScopedLock* leaveScopedLock();

        void lockedNestable( Lock::Nestable what , int type );
        void unlockedNestable();
        void lockedOther( const string& db , int type , WrapperForRWLock* lock );
        void unlockedOther();
    private:
        unsigned _recursive;           // we allow recursively asking for a lock; we track that here

        // global lock related
        char _threadState;             // 0, 'r', 'w', 'R', 'W'

        // db level locking related
        Lock::Nestable _whichNestable;
        int _nestableCount;            // recursive lock count on local or admin db XXX - change name
        
        int _otherCount;               //   >0 means write lock, <0 read lock - XXX change name
        string _otherName;             // which database are we locking and working with (besides local/admin) 
        WrapperForRWLock* _otherLock;  // so we don't have to check the map too often (the map has a mutex)

        // for temprelease
        // for the nonrecursive case. otherwise there would be many
        // the first lock goes here, which is ok since we can't yield recursive locks
        Lock::ScopedLock* _scopedLk;   
        
    };

}
