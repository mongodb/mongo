// @file d_concurrency.h

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/


// only used by mongod, thus the name ('d')
// (also used by dbtests test binary, which is running mongod test code)

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/lockstat.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/rwlock.h"

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
        
        static LockStat* globalLockStat();
        static LockStat* nestableLockStat( Nestable db );

        class ScopedLock;

        // note: avoid TempRelease when possible. not a good thing.
        struct TempRelease {
            TempRelease(); 
            ~TempRelease();
            const bool cant; // true if couldn't because of recursive locking
            ScopedLock *scopedLk;
        };

        /** turn on "parallel batch writer mode".  blocks all other threads. this mode is off
            by default. note only one thread creates a ParallelBatchWriterMode object; the rest just
            call iAmABatchParticipant().  Note that this lock is not released on a temprelease, just
            the normal lock things below.
            */
        class ParallelBatchWriterMode : boost::noncopyable {
            RWLockRecursive::Exclusive _lk;
        public:
            ParallelBatchWriterMode() : _lk(_batchLock) {}
            static void iAmABatchParticipant();
            static RWLockRecursive &_batchLock;
        };

    private:
        class ParallelBatchWriterSupport : boost::noncopyable {
        public:
            ParallelBatchWriterSupport();

        private:
            void tempRelease();
            void relock();

            scoped_ptr<RWLockRecursive::Shared> _lk;
            friend class ScopedLock;
        };

    public:
        class ScopedLock : boost::noncopyable {
        public:
            virtual ~ScopedLock();

            /** @return micros since we started acquiring */
            long long acquireFinished( LockStat* stat );

            // Accrue elapsed lock time since last we called reset
            void recordTime();
            // Start recording a new period, starting now()
            void resetTime();

        protected:
            explicit ScopedLock( char type ); 

        private:
            friend struct TempRelease;
            void tempRelease(); // TempRelease class calls these
            void relock();

        protected:
            virtual void _tempRelease() = 0;
            virtual void _relock() = 0;

        private:
            ParallelBatchWriterSupport _pbws_lk;

            void _recordTime( long long micros );
            Timer _timer;
            char _type;      // 'r','w','R','W'
            LockStat* _stat; // the stat for the relevant lock to increment when we're done
        };

        // note that for these classes recursive locking is ok if the recursive locking "makes sense"
        // i.e. you could grab globalread after globalwrite.
        
        class GlobalWrite : public ScopedLock {
            bool noop;
        protected:
            void _tempRelease();
            void _relock();
        public:
            // stopGreed is removed and does NOT work
            // timeoutms is only for writelocktry -- deprecated -- do not use
            GlobalWrite(bool stopGreed = false, int timeoutms = -1 ); 
            virtual ~GlobalWrite();
            void downgrade(); // W -> R
            void upgrade();   // caution see notes
        };
        class GlobalRead : public ScopedLock { // recursive is ok
        public:
            bool noop;
        protected:
            void _tempRelease();
            void _relock();
        public:
            // timeoutms is only for readlocktry -- deprecated -- do not use
            GlobalRead( int timeoutms = -1 ); 
            virtual ~GlobalRead();
        };

        // lock this database. do not shared_lock globally first, that is handledin herein. 
        class DBWrite : public ScopedLock {
            /**
             * flow
             *   1) lockDB
             *      a) lockTop
             *      b) lockNestable or lockOther
             *   2) unlockDB
             */

            void lockTop(LockState&);
            void lockNestable(Nestable db);
            void lockOther(const StringData& db);
            void lockDB(const string& ns);
            void unlockDB();

        protected:
            void _tempRelease();
            void _relock();

        public:
            DBWrite(const StringData& dbOrNs);
            virtual ~DBWrite();

            class UpgradeToExclusive : private boost::noncopyable {
            public:
                UpgradeToExclusive();
                ~UpgradeToExclusive();

                bool gotUpgrade() const { return _gotUpgrade; }
            private:
                bool _gotUpgrade;
            };

        private:
            bool _locked_w;
            bool _locked_W;
            WrapperForRWLock *_weLocked;
            const string _what;
            bool _nested;
        };

        // lock this database for reading. do not shared_lock globally first, that is handledin herein. 
        class DBRead : public ScopedLock {
            void lockTop(LockState&);
            void lockNestable(Nestable db);
            void lockOther(const StringData& db);
            void lockDB(const string& ns);
            void unlockDB();

        protected:
            void _tempRelease();
            void _relock();

        public:
            DBRead(const StringData& dbOrNs);
            virtual ~DBRead();

        private:
            bool _locked_r;
            WrapperForRWLock *_weLocked;
            string _what;
            bool _nested;
            
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


}
