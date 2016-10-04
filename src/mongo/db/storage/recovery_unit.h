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
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/snapshot_name.h"

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

    virtual void reportState(BSONObjBuilder* b) const {}

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
     * Waits until all commits that happened before this call are durable. Returns true, unless the
     * storage engine cannot guarantee durability, which should never happen when isDurable()
     * returned true. This cannot be called from inside a unit of work, and should fail if it is.
     */
    virtual bool waitUntilDurable() = 0;

    /**
     * When this is called, if there is an open transaction, it is closed. On return no
     * transaction is active. This cannot be called inside of a WriteUnitOfWork, and should
     * fail if it is.
     */
    virtual void abandonSnapshot() = 0;

    /**
     * Informs this RecoveryUnit that all future reads through it should be from a snapshot
     * marked as Majority Committed. Snapshots should still be separately acquired and newer
     * committed snapshots should be used if available whenever implementations would normally
     * change snapshots.
     *
     * If no snapshot has yet been marked as Majority Committed, returns a status with error
     * code ReadConcernMajorityNotAvailableYet. After this returns successfully, at any point where
     * implementations attempt to acquire committed snapshot, if there are none available due to a
     * call to SnapshotManager::dropAllSnapshots(), a UserException with the same code should be
     * thrown.
     *
     * StorageEngines that don't support a SnapshotManager should use the default
     * implementation.
     */
    virtual Status setReadFromMajorityCommittedSnapshot() {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority readConcerns"};
    }

    /**
     * Returns true if setReadFromMajorityCommittedSnapshot() has been called.
     */
    virtual bool isReadingFromMajorityCommittedSnapshot() const {
        return false;
    }

    /**
     * Returns the SnapshotName being used by this recovery unit or boost::none if not reading from
     * a majority committed snapshot.
     *
     * It is possible for reads to occur from later snapshots, but they may not occur from earlier
     * snapshots.
     */
    virtual boost::optional<SnapshotName> getMajorityCommittedSnapshot() const {
        dassert(!isReadingFromMajorityCommittedSnapshot());
        return {};
    }

    /**
     * Gets the local SnapshotId.
     *
     * It is only valid to compare SnapshotIds generated by a single RecoveryUnit.
     *
     * This is unrelated to SnapshotName which must be globally comparable.
     */
    virtual SnapshotId getSnapshotId() const = 0;

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
     */
    class Change {
    public:
        virtual ~Change() {}

        virtual void rollback() = 0;
        virtual void commit() = 0;
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
            void commit() final {}

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
            void commit() final {
                _callback();
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

protected:
    RecoveryUnit() {}
};

}  // namespace mongo
