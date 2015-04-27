/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/decorable.h"

namespace mongo {

    class Client;
    class CurOp;
    class ProgressMeter;
    class StringData;
    /**
     * This class encompasses the state required by an operation.
     *
     * TODO(HK): clarify what this means.  There's one OperationContext for one user operation...
     *           but is this true for getmore?  Also what about things like fsyncunlock / internal
     *           users / etc.?
     *
     * On construction, an OperationContext associates itself with the current client, and only on
     * destruction it deassociates itself. At any time a client can be associated with at most one
     * OperationContext.
     */
    class OperationContext : public Decorable<OperationContext> {
        MONGO_DISALLOW_COPYING(OperationContext);
    public:
        virtual ~OperationContext() { }

        /**
         * Interface for durability.  Caller DOES NOT own pointer.
         */
        virtual RecoveryUnit* recoveryUnit() const = 0;

        /**
         * Returns the RecoveryUnit (same return value as recoveryUnit()) but the caller takes
         * ownership of the returned RecoveryUnit, and the OperationContext instance relinquishes
         * ownership.  Sets the RecoveryUnit to NULL.
         *
         * Used to transfer ownership of storage engine state from OperationContext
         * to ClientCursor for getMore-able queries.
         *
         * Note that we don't allow the top-level locks to be stored across getMore.
         * We rely on active cursors being killed when collections or databases are dropped,
         * or when collection metadata changes.
         */
        virtual RecoveryUnit* releaseRecoveryUnit() = 0;

        virtual void setRecoveryUnit(RecoveryUnit* unit) = 0;

        /**
         * Interface for locking.  Caller DOES NOT own pointer.
         */
        virtual Locker* lockState() const = 0;

        // --- operation level info? ---

        /**
         * If the thread is not interrupted, returns Status::OK(), otherwise returns the cause
         * for the interruption. The throw variant returns a user assertion corresponding to the
         * interruption status.
         */
        virtual void checkForInterrupt() const = 0;
        virtual Status checkForInterruptNoAssert() const = 0;

        /**
         * Delegates to CurOp, but is included here to break dependencies.
         * Caller does not own the pointer.
         */
        virtual ProgressMeter* setMessage(const char* msg,
                                          const std::string& name = "Progress",
                                          unsigned long long progressMeterTotal = 0,
                                          int secondsBetween = 3) = 0;

        /**
         * Delegates to CurOp, but is included here to break dependencies.
         *
         * TODO: We return a string because of hopefully transient CurOp thread-unsafe insanity.
         */
        virtual std::string getNS() const = 0;

        /**
         * Returns the client under which this context runs.
         */
        virtual Client* getClient() const = 0;

        /**
         * Returns CurOp. Caller does not own pointer
         */
        virtual CurOp* getCurOp() const = 0;

        /**
         * Returns the operation ID associated with this operation.
         * WARNING: Due to SERVER-14995, this OpID is not guaranteed to stay the same for the
         * lifetime of this OperationContext.
         */
        virtual unsigned int getOpID() const = 0;

        /**
         * @return true if this instance is primary for this namespace
         */
        virtual bool isPrimaryFor( StringData ns ) = 0;

        /**
         * Returns WriteConcernOptions of the current operation
         */
        const WriteConcernOptions& getWriteConcern() const {
            return _writeConcern;
        }

        void setWriteConcern(const WriteConcernOptions& writeConcern) {
            _writeConcern = writeConcern;
        }

        /**
         * Set whether or not operations should generate oplog entries.
         */
        virtual void setReplicatedWrites(bool writesAreReplicated = true) = 0;

        /**
         * Returns true if operations should generate oplog entries.
         */
        virtual bool writesAreReplicated() const = 0;

    protected:
        OperationContext() { }

    private:
        WriteConcernOptions _writeConcern;
    };

    class WriteUnitOfWork {
        MONGO_DISALLOW_COPYING(WriteUnitOfWork);
    public:
        WriteUnitOfWork(OperationContext* txn)
                 : _txn(txn),
                   _ended(false) {

            _txn->lockState()->beginWriteUnitOfWork();
            _txn->recoveryUnit()->beginUnitOfWork(_txn);
        }

        ~WriteUnitOfWork() {
            _txn->recoveryUnit()->endUnitOfWork();

            if (!_ended) {
                _txn->lockState()->endWriteUnitOfWork();
            }
        }

        void commit() {
            invariant(!_ended);

            _txn->recoveryUnit()->commitUnitOfWork();
            _txn->lockState()->endWriteUnitOfWork();

            _ended = true;
        }

    private:
        OperationContext* const _txn;

        bool _ended;
    };


    /**
     * RAII-style class to mark the scope of a transaction. ScopedTransactions may be nested.
     * An outermost ScopedTransaction calls commitAndRestart() on destruction, so that the storage
     * engine can release resources, such as snapshots or locks, that it may have acquired during
     * the transaction. Note that any writes are committed in nested WriteUnitOfWork scopes,
     * so write conflicts cannot happen on completing a ScopedTransaction.
     *
     * TODO: The ScopedTransaction should hold the global lock
     */
    class ScopedTransaction {
        MONGO_DISALLOW_COPYING(ScopedTransaction);
    public:
        /**
         * The mode for the transaction indicates whether the transaction will write (MODE_IX) or
         * only read (MODE_IS), or needs to run without other writers (MODE_S) or any other
         * operations (MODE_X) on the server.
         */
        ScopedTransaction(OperationContext* txn, LockMode mode) : _txn(txn) { }

        ~ScopedTransaction() {
            if (!_txn->lockState()->isLocked()) {
                _txn->recoveryUnit()->commitAndRestart();
            }
        }

    private:
        OperationContext* _txn;
    };

}  // namespace mongo
