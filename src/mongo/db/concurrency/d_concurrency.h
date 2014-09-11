/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#include <boost/scoped_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/lock_stat.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/rwlock.h"


namespace mongo {

    class Locker;

    class Lock : boost::noncopyable { 
    public:
        class ScopedLock;

        // note: avoid TempRelease when possible. not a good thing.
        struct TempRelease {
            TempRelease(Locker* lockState);
            ~TempRelease();
            const bool cant; // true if couldn't because of recursive locking

            // Not owned
            Locker* _lockState;
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
            static void iAmABatchParticipant(Locker* lockState);
            static RWLockRecursive &_batchLock;
        };

    public:

        class ScopedLock : boost::noncopyable {
        public:
            virtual ~ScopedLock();

            // Start recording a new period, starting now()
            void resetTime();

            // Accrue elapsed lock time since last we called reset
            void recordTime();

        protected:
            explicit ScopedLock(Locker* lockState, char type );

        private:
            friend struct TempRelease;

            void tempRelease(); // TempRelease class calls these
            void relock();

        protected:
            virtual void _tempRelease() = 0;
            virtual void _relock() = 0;

            Locker* _lockState;

        private:

            class ParallelBatchWriterSupport : boost::noncopyable {
            public:
                ParallelBatchWriterSupport(Locker* lockState);

            private:
                void tempRelease();
                void relock();

                Locker* _lockState;
                boost::scoped_ptr<RWLockRecursive::Shared> _lk;
                friend class ScopedLock;
            };

            ParallelBatchWriterSupport _pbws_lk;

            Timer _timer;
            char _type;      // 'r','w','R','W'
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
            GlobalWrite(Locker* lockState, int timeoutms = -1);
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
            GlobalRead(Locker* lockState, int timeoutms = -1);
            virtual ~GlobalRead();
        };

        // lock this database. do not shared_lock globally first, that is handledin herein. 
        class DBWrite : public ScopedLock {
            void lockTop();
            void lockDB();
            void unlockDB();

        protected:
            void _tempRelease();
            void _relock();

        public:
            DBWrite(Locker* lockState, const StringData& dbOrNs);
            virtual ~DBWrite();

        private:
            bool _lockAcquired;
            const std::string _ns;
        };

        // lock this database for reading. do not shared_lock globally first, that is handledin herein. 
        class DBRead : public ScopedLock {
            void lockTop();
            void lockDB();
            void unlockDB();

        protected:
            void _tempRelease();
            void _relock();

        public:
            DBRead(Locker* lockState, const StringData& dbOrNs);
            virtual ~DBRead();

        private:
            bool _lockAcquired;
            const std::string _ns;
        };

        /**
         * Acquires a previously acquired intent-X (lower-case 'w') GlobalWrite lock to upper-case
         * 'W' lock. Effectively means "stop the world".
         */
        class UpgradeGlobalLockToExclusive : private boost::noncopyable {
        public:
            UpgradeGlobalLockToExclusive(Locker* lockState);
            ~UpgradeGlobalLockToExclusive();

            bool gotUpgrade() const { return _gotUpgrade; }

        private:
            Locker* _lockState;
            bool _gotUpgrade;
        };
    };

    class readlocktry : boost::noncopyable {
        bool _got;
        boost::scoped_ptr<Lock::GlobalRead> _dbrlock;
    public:
        readlocktry(Locker* lockState, int tryms);
        ~readlocktry();
        bool got() const { return _got; }
    };

    class writelocktry : boost::noncopyable {
        bool _got;
        boost::scoped_ptr<Lock::GlobalWrite> _dbwlock;
    public:
        writelocktry(Locker* lockState, int tryms);
        ~writelocktry();
        bool got() const { return _got; }
    };
}
