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
#include "mongo/db/concurrency/lock_mgr_defs.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/rwlock.h"
#include "mongo/util/timer.h"

namespace mongo {

    class Locker;
    class StringData;

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

            // TempRelease class calls these
            void tempRelease();
            void relock();

        protected:
            virtual void _tempRelease();
            virtual void _relock();

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

            char _type;      // 'r','w','R','W'
        };

        /**
         * Global exclusive lock
         *
         * Allows exclusive write access to all databases and collections, blocking all other
         * access. Allows further (recursive) acquisition of the global lock in any mode,
         * see LockMode.
         */
        class GlobalWrite : public ScopedLock {
        protected:
            void _tempRelease();
            void _relock();
        public:
            // timeBudgetRemaining is only for writelocktry -- deprecated -- do not use
            GlobalWrite(Locker* lockState, Microseconds* timeBudgetRemaining = NULL);
            virtual ~GlobalWrite();
        };

        /**
         * Global shared lock
         *
         * Allows concurrent read access to all databases and collections, blocking any writers.
         * Allows further (recursive) acquisition of the global lock in shared (S) or intent-shared
         * (IS) mode, see LockMode.
         */
        class GlobalRead : public ScopedLock {
        public:
            // timeBudgetRemaining is only for readlocktry -- deprecated -- do not use
            GlobalRead(Locker* lockState, Microseconds* timeBudgetRemaining = NULL);
            virtual ~GlobalRead();
        };

        /**
         * Database lock with support for collection- and document-level locking
         *
         * This lock supports four modes (see Lock_Mode):
         *   MODE_IS: concurrent database access, requiring further collection read locks
         *   MODE_IX: concurrent database access, requiring further collection read or write locks
         *   MODE_S:  shared read access to the database, blocking any writers
         *   MODE_X:  exclusive access to the database, blocking all other readers and writers
         *
         * For MODE_IS or MODE_S also acquires global lock in intent-shared (IS) mode, and
         * for MODE_IX or MODE_X also acquires global lock in intent-exclusive (IX) mode.
         * For storage engines that do not support collection-level locking, MODE_IS will be
         * upgraded to MODE_S and MODE_IX will be upgraded to MODE_X.
         */
        class DBLock : public ScopedLock {
        public:
            DBLock(Locker* lockState, const StringData& db, const LockMode mode);
            virtual ~DBLock();

            /**
             * Releases the DBLock and reacquires it with the new mode. The global intent
             * lock is retained (so the database can't disappear). Relocking from MODE_IS or
             * MODE_S to MODE_IX or MODE_X is not allowed to avoid violating the global intent.
             * Use relockWithMode() instead of upgrading to avoid deadlock.
             */
            void relockWithMode(const LockMode newMode);

        private:
            void lockDB();
            void unlockDB();

            const ResourceId _id;
            LockMode _mode; // may be changed through relockWithMode

        protected:
            // Still need to override these for ScopedLock::tempRelease() and relock().
            // TODO: make this go away
            void _tempRelease()  { unlockDB(); }
            void _relock() { lockDB(); }
        };

        /**
         * Collection lock with support for document-level locking
         *
         * This lock supports four modes (see Lock_Mode):
         *   MODE_IS: concurrent collection access, requiring document level locking read locks
         *   MODE_IX: concurrent collection access, requiring document level read or write locks
         *   MODE_S:  shared read access to the collection, blocking any writers
         *   MODE_X:  exclusive access to the collection, blocking all other readers and writers
         *
         * An appropriate DBLock must already be held before locking a collection: it is an error,
         * checked with a dassert(), to not have a suitable database lock before locking the
         * collection. For storage engines that do not support document-level locking, MODE_IS
         * will be upgraded to MODE_S and MODE_IX will be upgraded to MODE_X.
         */
        class CollectionLock : boost::noncopyable {
        public:
            CollectionLock(Locker* lockState, const StringData& ns, const LockMode);
            virtual ~CollectionLock();

            void relockWithMode( const LockMode mode, Lock::DBLock& dblock );
        private:
            const ResourceId _id;
            Locker* _lockState;
        };

        /**
         * General purpose RAII wrapper for a resource managed by the lock manager
         *
         * See LockMode for the supported modes. Unlike DBLock/Collection lock, this will not do
         * any additional checks/upgrades or global locking. Use ResourceLock for locking
         * resources other than RESOURCE_GLOBAL, RESOURCE_DATABASE and RESOURCE_COLLECTION.
         */
        class ResourceLock : boost::noncopyable {
        public:
            ResourceLock(Locker* lockState, const ResourceId rid, const LockMode);
            virtual ~ResourceLock();
        private:
            const ResourceId _rid;
            Locker* _lockState;
        };

        /**
         * Shared database lock -- DEPRECATED, please transition to DBLock and collection locks
         *
         * Allows concurrent read access to the given database, blocking any writers.
         * Allows further (recursive) acquisision of database locks for this database in shared
         * or intent-shared mode. Also acquires global lock in intent-shared (IS) mode.
         */
        class DBRead : public DBLock {
        public:
            DBRead(Locker* lockState, const StringData& dbOrNs);
        };
    };

    class DBTryLockTimeoutException : public std::exception {
    public:
        DBTryLockTimeoutException();
        virtual ~DBTryLockTimeoutException() throw();
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
