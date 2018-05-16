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

#include <cstdint>
#include <stdlib.h>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/snapshot.h"

namespace mongo {

class BSONObjBuilder;
class OperationContext;

/**
 * A RecoveryUnit is responsible for ensuring that data is persisted.
 * All on-disk information must be mutated through this interface.
 */
class RecoveryUnit {
    MONGO_DISALLOW_COPYING(RecoveryUnit);

public:
    virtual ~RecoveryUnit() {}

    /**
     * These should be called through WriteUnitOfWork rather than directly.
     *
     * A call to 'beginUnitOfWork' marks the beginning of a unit of work. Each call to
     * 'beginUnitOfWork' must be matched with exactly one call to either 'commitUnitOfWork' or
     * 'abortUnitOfWork'. When 'abortUnitOfWork' is called, all changes made since the begin
     * of the unit of work will be rolled back.
     */
    virtual void beginUnitOfWork(OperationContext* opCtx) = 0;
    virtual void commitUnitOfWork() = 0;
    virtual void abortUnitOfWork() = 0;

    /**
     * Must be called after beginUnitOfWork and before calling either abortUnitOfWork or
     * commitUnitOfWork. Transitions the current transaction (unit of work) to the
     * "prepared" state. Must be overridden by storage engines that support prepared
     * transactions.
     *
     * Must be preceded by a call to setPrepareTimestamp().
     *
     * It is not valid to call commitUnitOfWork() afterward without calling setCommitTimestamp()
     * with a value greater than or equal to the prepare timestamp.
     * This cannot be called after setTimestamp or setCommitTimestamp.
     */
    virtual void prepareUnitOfWork() {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support prepared transactions");
    }

    virtual void setIgnorePrepared(bool ignore) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support prepared transactions");
    }

    /**
     * Waits until all commits that happened before this call are durable in the journal. Returns
     * true, unless the storage engine cannot guarantee durability, which should never happen when
     * isDurable() returned true. This cannot be called from inside a unit of work, and should
     * fail if it is.
     */
    virtual bool waitUntilDurable() = 0;

    /**
     * Unlike `waitUntilDurable`, this method takes a stable checkpoint, making durable any writes
     * on unjournaled tables that are behind the current stable timestamp. If the storage engine
     * is starting from an "unstable" checkpoint, this method call will turn into an unstable
     * checkpoint.
     *
     * This must not be called by a system taking user writes until after a stable timestamp is
     * passed to the storage engine.
     */
    virtual bool waitUntilUnjournaledWritesDurable() {
        return waitUntilDurable();
    }

    /**
     * When this is called, if there is an open transaction, it is closed. On return no
     * transaction is active. This cannot be called inside of a WriteUnitOfWork, and should
     * fail if it is.
     */
    virtual void abandonSnapshot() = 0;

    /**
     * Informs the RecoveryUnit that a snapshot will be needed soon, if one was not already
     * established. This specifically allows the storage engine to preallocate any required
     * transaction resources while minimizing the critical section between generating a new
     * timestamp and setting it using setTimestamp.
     */
    virtual void preallocateSnapshot() {}

    /**
     * Obtains a majority committed snapshot. Snapshots should still be separately acquired and
     * newer committed snapshots should be used if available whenever implementations would normally
     * change snapshots.
     *
     * If no snapshot has yet been marked as Majority Committed, returns a status with error code
     * ReadConcernMajorityNotAvailableYet. After this returns successfully, at any point where
     * implementations attempt to acquire committed snapshot, if there are none available due to a
     * call to SnapshotManager::dropAllSnapshots(), a AssertionException with the same code should
     * be thrown.
     *
     * StorageEngines that don't support a SnapshotManager should use the default
     * implementation.
     */
    virtual Status obtainMajorityCommittedSnapshot() {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority readConcerns"};
    }

    /**
     * Returns the Timestamp being used by this recovery unit or boost::none if not reading from
     * a point in time. Any point in time returned will reflect one of the following:
     *  - when using ReadSource::kProvided, the timestamp provided.
     *  - when using ReadSource::kLastAppliedSnapshot, the timestamp chosen using the storage
     * engine's last applied timestamp.
     *  - when using ReadSource::kLastApplied, the last applied timestamp at which the current
     * storage transaction was opened, if one is open.
     *  - when using ReadSource::kMajorityCommitted, the majority committed timestamp chosen by the
     * storage engine after a transaction has been opened or after a call to
     * obtainMajorityCommittedSnapshot().
     */
    virtual boost::optional<Timestamp> getPointInTimeReadTimestamp() const {
        return boost::none;
    }

    /**
     * Gets the local SnapshotId.
     *
     * It is only valid to compare SnapshotIds generated by a single RecoveryUnit.
     *
     * This is unrelated to Timestamp which must be globally comparable.
     */
    virtual SnapshotId getSnapshotId() const = 0;

    /**
     * Sets a timestamp to assign to future writes in a transaction.
     * All subsequent writes will be assigned this timestamp.
     * If setTimestamp() is called again, specifying a new timestamp, future writes will use this
     * new timestamp but past writes remain with their originally assigned timestamps.
     * Writes that occur before any setTimestamp() is called will be assigned the timestamp
     * specified in the last setTimestamp() call in the transaction, at commit time.
     *
     * setTimestamp() will fail if a commit timestamp is set using setCommitTimestamp() and not
     * yet cleared with clearCommitTimestamp().
     */
    virtual Status setTimestamp(Timestamp timestamp) {
        return Status::OK();
    }

    /**
     * Sets a timestamp that will be assigned to all future writes on this RecoveryUnit until
     * clearCommitTimestamp() is called. This must be called outside of a WUOW and setTimestamp()
     * must not be called while a commit timestamp is set.
     */
    virtual void setCommitTimestamp(Timestamp timestamp) {}

    virtual void clearCommitTimestamp() {}

    virtual Timestamp getCommitTimestamp() {
        return {};
    }

    /**
     * Sets a prepare timestamp for the current transaction. A subsequent call to
     * prepareUnitOfWork() is expected and required.
     * This cannot be called after setTimestamp or setCommitTimestamp.
     * This must be called inside a WUOW and may only be called once.
     */
    virtual void setPrepareTimestamp(Timestamp timestamp) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support prepared transactions");
    }

    /**
     * When no read timestamp is provided to the recovery unit, the ReadSource indicates which
     * external timestamp source to read from.
     */
    enum ReadSource {
        kNone,               // Do not read from a timestamp. This is the default.
        kMajorityCommitted,  // Read from the majority all-commmitted timestamp.
        kLastApplied,  // Read from the last applied timestamp. New transactions start at the most
                       // up-to-date timestamp.
        kLastAppliedSnapshot,  // Read from the last applied timestamp. New transactions will always
                               // read from the same timestamp and never advance.
        kProvided              // Read from the timestamp provided to setTimestampReadSource.
    };

    /**
     * Sets which timestamp to use for read transactions. If 'provided' is supplied, only kProvided
     * is an acceptable input.
     *
     * Must be called in one of the following cases:
     * - a transaction is not active
     * - no read source has been set yet
     * - the read source provided is the same as the existing read source
     */
    virtual void setTimestampReadSource(ReadSource source,
                                        boost::optional<Timestamp> provided = boost::none) {}

    virtual ReadSource getTimestampReadSource() const {
        return ReadSource::kNone;
    };

    /**
     * A Change is an action that is registerChange()'d while a WriteUnitOfWork exists. The
     * change is either rollback()'d or commit()'d when the WriteUnitOfWork goes out of scope.
     *
     * Neither rollback() nor commit() may fail or throw exceptions.
     *
     * Change implementors are responsible for handling their own locking, and must be aware
     * that rollback() and commit() may be called after resources with a shorter lifetime than
     * the WriteUnitOfWork have been freed. Each registered change will be committed or rolled
     * back once.
     *
     * commit() handlers are passed the timestamp at which the transaction is committed. If the
     * transaction is not committed at a particular timestamp, or if the storage engine does not
     * support timestamps, then boost::none will be supplied for this parameter.
     */
    class Change {
    public:
        virtual ~Change() {}

        virtual void rollback() = 0;
        virtual void commit(boost::optional<Timestamp> commitTime) = 0;
    };

    /**
     * The RecoveryUnit takes ownership of the change. The commitUnitOfWork() method calls the
     * commit() method of each registered change in order of registration. The endUnitOfWork()
     * method calls the rollback() method of each registered Change in reverse order of
     * registration. Either will unregister and delete the changes.
     *
     * The registerChange() method may only be called when a WriteUnitOfWork is active, and
     * may not be called during commit or rollback.
     */
    virtual void registerChange(Change* change) = 0;

    /**
     * Registers a callback to be called if the current WriteUnitOfWork rolls back.
     *
     * Be careful about the lifetimes of all variables captured by the callback!
     */
    template <typename Callback>
    void onRollback(Callback callback) {
        class OnRollbackChange final : public Change {
        public:
            OnRollbackChange(Callback&& callback) : _callback(std::move(callback)) {}
            void rollback() final {
                _callback();
            }
            void commit(boost::optional<Timestamp>) final {}

        private:
            Callback _callback;
        };

        registerChange(new OnRollbackChange(std::move(callback)));
    }

    /**
     * Registers a callback to be called if the current WriteUnitOfWork commits.
     *
     * Be careful about the lifetimes of all variables captured by the callback!
     */
    template <typename Callback>
    void onCommit(Callback callback) {
        class OnCommitChange final : public Change {
        public:
            OnCommitChange(Callback&& callback) : _callback(std::move(callback)) {}
            void rollback() final {}
            void commit(boost::optional<Timestamp> commitTime) final {
                _callback(commitTime);
            }

        private:
            Callback _callback;
        };

        registerChange(new OnCommitChange(std::move(callback)));
    }

    //
    // The remaining methods probably belong on DurRecoveryUnit rather than on the interface.
    //

    /**
     * Declare that the data at [x, x + len) is being written.
     */
    virtual void* writingPtr(void* data, size_t len) = 0;

    //
    // Syntactic sugar
    //

    /**
     * Declare write intent for an int
     */
    inline int& writingInt(int& d) {
        return *writing(&d);
    }

    /**
     * A templated helper for writingPtr.
     */
    template <typename T>
    inline T* writing(T* x) {
        writingPtr(x, sizeof(T));
        return x;
    }

    /**
     * Sets a flag that declares this RecoveryUnit will skip rolling back writes, for the
     * duration of the current outermost WriteUnitOfWork.  This function can only be called
     * between a pair of unnested beginUnitOfWork() / endUnitOfWork() calls.
     * The flag is cleared when endUnitOfWork() is called.
     * While the flag is set, rollback will skip rolling back writes, but custom rollback
     * change functions are still called.  Clearly, this functionality should only be used when
     * writing to temporary collections that can be cleaned up externally.  For example,
     * foreground index builds write to a temporary collection; if something goes wrong that
     * normally requires a rollback, we can instead clean up the index by dropping the entire
     * index.
     * Setting the flag may permit increased performance.
     */
    virtual void setRollbackWritesDisabled() = 0;

    virtual void setOrderedCommit(bool orderedCommit) = 0;

protected:
    RecoveryUnit() {}
};

}  // namespace mongo
