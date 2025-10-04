/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/storage_metrics.h"
#include "mongo/db/storage/storage_stats.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObjBuilder;

class OperationContext;

/**
 * The PrepareConflictBehavior specifies how operations should behave when encountering prepare
 * conflicts.
 */
enum class PrepareConflictBehavior {
    /**
     * When prepare conflicts are encountered, block until the conflict is resolved.
     */
    kEnforce = 0,

    /**
     * Ignore prepare conflicts when they are encountered.
     *
     * When a prepared update is encountered, the previous version of a record will be returned.
     * This behavior can result in reading different versions of a record within the same snapshot
     * if the prepared update is committed during that snapshot. For this reason, operations that
     * ignore prepared updates may only perform reads. This is to prevent updating a record based on
     * an older version of itself, because a write conflict will not be generated in this scenario.
     */
    kIgnoreConflicts,

    /**
     * Ignore prepare conflicts when they are encountered, and allow operations to perform writes,
     * an exception to the rule of kIgnoreConflicts.
     *
     * This should only be used in cases where this is known to be impossible to perform writes
     * based on other prepared updates.
     */
    kIgnoreConflictsAllowWrites,

    /** kMax should always be last and is a counter of the number of enum values. */
    kMax,
};

/**
 * DataCorruptionDetectionMode determines how we handle the discovery of evidence of data
 * corruption.
 */
enum class DataCorruptionDetectionMode {
    /**
     * Always throw a DataCorruptionDetected error when evidence of data corruption is detected.
     */
    kThrow,
    /**
     * When evidence of data corruption is decected, log an entry to the health log and the
     * server logs, but do not throw an error. Continue attempting to return results.
     */
    kLogAndContinue,
};

/**
 * A RecoveryUnit is responsible for ensuring that data is persisted.
 * All on-disk information must be mutated through this interface.
 */
class RecoveryUnit {
    RecoveryUnit(const RecoveryUnit&) = delete;
    RecoveryUnit& operator=(const RecoveryUnit&) = delete;

public:
    enum class Isolation {
        readUncommitted,
        readCommitted,
        snapshot,
    };

    virtual ~RecoveryUnit() = default;

    /**
     * A Snapshot is a decorable type whose lifetime is tied to the the lifetime of a
     * snapshot within the RecoveryUnit. Snapshots hold no storage engine state and are to
     * be used for snapshot ID comparison on a single RecoveryUnit and to support decorated
     * types that should be destructed when the storage snapshot is invalidated.
     *
     * Classes that decorate a Snapshot are constructed before a new storage snapshot is
     * established and destructed after the storage engine snapshot has been released.
     */
    class Snapshot : public Decorable<Snapshot> {
    public:
        explicit Snapshot(SnapshotId id) : _id(id) {}
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        Snapshot(Snapshot&&) = default;
        Snapshot& operator=(Snapshot&&) = default;

        SnapshotId getId() const {
            return _id;
        }

    private:
        SnapshotId _id;
    };

    /**
     * Returns the current Snapshot on this RecoveryUnit, constructing one if none exists yet.
     * Otherwise, a Snapshot is guaranteed to be constructed when the storage engine snapshot is
     * opened. Will be destructed when the storage engine snapshot is closed via calls to
     * abandonSnapshot, commitUnitOfWork, or abortUnitOfWork.
     *
     * Note that the RecoveryUnit does not make any guarantees that this reference remains valid
     * except for the lifetime of the Snapshot.
     */
    Snapshot& getSnapshot() {
        if (!_snapshot.is_initialized()) {
            ensureSnapshot();
        }
        return _snapshot.get();
    }

    // Behavior for abandonSnapshot().
    enum class AbandonSnapshotMode {
        kAbort,  // default
        kCommit
    };

    // Specifies the level of suppression of untimestamped writes errors.
    enum class UntimestampedWriteAssertionLevel {
        kSuppressOnce,    // Suppress errors throughout one write unit of work.
        kSuppressAlways,  // Suppress errors throughout the lifetime of the RecoveryUnit.
        kEnforce          // Enforce untimestamped writes errors (this is the default).
    };

    void commitRegisteredChanges(boost::optional<Timestamp> commitTimestamp);
    void abortRegisteredChanges();

    /**
     * Marks the beginning of a unit of work. Each call must be matched with exactly one call to
     * either commitUnitOfWork or abortUnitOfWork.
     *
     * When called with readOnly=true, calls to commitUnitOfWork and abortUnitOfWork will apply any
     * registered changes and reset the readOnly state, but no writes performed in this unit of work
     * will be committed.
     *
     * Should be called through WriteUnitOfWork rather than directly.
     */
    void beginUnitOfWork(bool readOnly);

    /**
     * Marks the end of a unit of work and commits all changes registered by calls to onCommit or
     * registerChange, in order. Must be matched by exactly one preceding call to beginUnitOfWork.
     *
     * Should be called through WriteUnitOfWork rather than directly.
     */
    void commitUnitOfWork();

    /**
     * Marks the end of a unit of work and rolls back all changes registered by calls to onRollback
     * or registerChange, in reverse order. Must be matched by exactly one preceding call to
     * beginUnitOfWork.
     *
     * Should be called through WriteUnitOfWork rather than directly.
     */
    void abortUnitOfWork();

    /**
     * Returns whether or not this RecoveryUnit is in a read-only unit of work. In this state,
     * users may assert when this RecoveryUnit is used for writing to the storage engine.
     */
    bool readOnly() const {
        return _readOnly;
    }

    /**
     * Sets whether cursors in this operation should engage in pre-fetching data from disk to
     * the storage engine cache. This feature can be useful for operations that are typically
     * I/O bound.
     */
    virtual void setPrefetching(bool enable) {}

    /**
     * Transitions the active unit of work to the "prepared" state. Must be called after
     * beginUnitOfWork and before calling either abortUnitOfWork or commitUnitOfWork. Must be
     * overridden by storage engines that support prepared transactions.
     *
     * Must be preceded by a call to beginUnitOfWork and  setPrepareTimestamp, in that order.
     *
     * This cannot be called after setTimestamp or setCommitTimestamp.
     */
    virtual void prepareUnitOfWork() {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support prepared transactions");
    }

    /**
     * Sets the behavior of handling conflicts that are encountered due to prepared transactions, if
     * supported by this storage engine. See PrepareConflictBehavior.
     */
    virtual void setPrepareConflictBehavior(PrepareConflictBehavior behavior) {}

    /**
     * Returns the behavior of handling conflicts that are encountered due to prepared transactions.
     * Defaults to kEnforce if prepared transactions are not supported by this storage engine.
     */
    virtual PrepareConflictBehavior getPrepareConflictBehavior() const {
        return PrepareConflictBehavior::kEnforce;
    }

    /**
     * If there is an open transaction, it is closed. If the current AbandonSnapshotMode is
     * 'kAbort', the transaction is aborted. If the mode is 'kCommit' the transaction is committed,
     * and all data currently pointed to by cursors remains pinned until the cursors are
     * repositioned.

     * On return no transaction is active. It is a programming error to call this inside of a
     * WriteUnitOfWork, even if the AbandonSnapshotMode is 'kCommit'.
     */
    void abandonSnapshot();

    void setAbandonSnapshotMode(AbandonSnapshotMode mode) {
        _abandonSnapshotMode = mode;
    }
    AbandonSnapshotMode abandonSnapshotMode() const {
        return _abandonSnapshotMode;
    }

    /**
     * Sets the OperationContext that currently owns this RecoveryUnit. Should only be called by the
     * OperationContext.
     */
    virtual void setOperationContext(OperationContext* opCtx);

    /**
     * Extensible structure for configuring options to begin a new transaction.
     *
     * - roundUpPreparedTimestamps dictates whether to round up prepare and commit timestamp of a
     * prepared transaction. If set to true, the prepare timestamp will be rounded up to the oldest
     * timestamp if found to be earlier; and the commit timestamp will be rounded up to the prepare
     * timestamp if found to be earlier.
     */
    struct OpenSnapshotOptions {
        bool roundUpPreparedTimestamps = false;

        bool operator==(const OpenSnapshotOptions& other) const = default;
    };
    static const OpenSnapshotOptions kDefaultOpenSnapshotOptions;

    /**
     * Informs the RecoveryUnit that a snapshot will be needed soon, if one was not already
     * established. This specifically allows the storage engine to preallocate any required
     * transaction resources while minimizing the critical section between generating a new
     * timestamp and setting it using setTimestamp.
     *
     * Non default options can be configured before a transaction begins. However, if a transaction
     * is already open, attempting to change the options is forbidden.
     */
    virtual void preallocateSnapshot(
        const OpenSnapshotOptions& options = kDefaultOpenSnapshotOptions) {}

    /**
     * Returns whether or not a majority commmitted snapshot is available. If no snapshot has yet
     * been marked as Majority Committed, returns a status with error code
     * ReadConcernMajorityNotAvailableYet. After this returns successfully, at any point where
     * implementations attempt to acquire committed snapshot, if there are none available due to a
     * call to SnapshotManager::clearCommittedSnapshot(), a AssertionException with the same code
     * should be thrown.
     *
     * StorageEngines that don't support a SnapshotManager should use the default
     * implementation.
     */
    virtual Status majorityCommittedSnapshotAvailable() const {
        return {ErrorCodes::CommandNotSupported,
                "Current storage engine does not support majority readConcerns"};
    }

    /**
     * Returns the Timestamp being used by this recovery unit or boost::none if not reading from
     * a point in time. Any point in time returned will reflect one of the following:
     *  - when using ReadSource::kProvided, the timestamp provided.
     *  - when using ReadSource::kNoOverlap, the timestamp chosen by the storage engine.
     *  - when using ReadSource::kAllDurableSnapshot, the timestamp chosen using the storage
     * engine's all_durable timestamp.
     *  - when using ReadSource::kLastAppplied, the last applied timestamp. Can return boost::none
     * if no timestamp has been established.
     *  - when using ReadSource::kMajorityCommitted, the majority committed timestamp chosen by the
     * storage engine after a transaction has been opened.
     *
     * This may passively start a storage engine transaction to establish a read timestamp.
     */
    virtual boost::optional<Timestamp> getPointInTimeReadTimestamp() {
        return boost::none;
    }

    /**
     * Gets the local SnapshotId.
     *
     * It is only valid to compare SnapshotIds generated by a single RecoveryUnit.
     *
     * This is unrelated to Timestamp which must be globally comparable.
     */
    SnapshotId getSnapshotId() {
        return getSnapshot().getId();
    }

    /**
     * Sets a timestamp to assign to future writes in a transaction.
     * All subsequent writes will be assigned this timestamp.
     * If setTimestamp() is called again, specifying a new timestamp, future writes will use this
     * new timestamp but past writes remain with their originally assigned timestamps.
     * Writes that occur before any setTimestamp() is called will be assigned the timestamp
     * specified in the last setTimestamp() call in the transaction, at commit time.
     *
     * setTimestamp() will fail if a commit timestamp is set using setCommitTimestamp() and not
     * yet cleared with clearCommitTimestamp(). setTimestamp() will also fail if a prepareTimestamp
     * has been set.
     */
    virtual Status setTimestamp(Timestamp timestamp) {
        return Status::OK();
    }

    /**
     * Returns true if a commit timestamp has been assigned to writes in this transaction.
     * Otherwise, returns false.
     */
    virtual bool isTimestamped() const {
        return false;
    }

    /**
     * Sets a timestamp that will be assigned to all future writes on this RecoveryUnit until
     * clearCommitTimestamp() is called. This must be called either outside of a WUOW or on a
     * prepared transaction after setPrepareTimestamp() is called. setTimestamp() must not be called
     * while a commit timestamp is set.
     */
    virtual void setCommitTimestamp(Timestamp timestamp) {}

    /**
     * Sets a timestamp that decides when all the future writes on this RecoveryUnit will be
     * durable.
     */
    virtual void setDurableTimestamp(Timestamp timestamp) {}

    /**
     * Clears the commit timestamp that was set by setCommitTimestamp(). This must be called outside
     * of a WUOW. This must be called when a commit timestamp is set.
     */
    virtual void clearCommitTimestamp() {}

    /**
     * Returns the commit timestamp. Can be called at any time.
     */
    virtual Timestamp getCommitTimestamp() const {
        return {};
    }

    /**
     * Returns the durable timestamp.
     */
    virtual Timestamp getDurableTimestamp() const {
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
     * Returns the prepare timestamp for the current transaction.
     * Must be called after setPrepareTimestamp(), and cannot be called after setTimestamp() or
     * setCommitTimestamp(). This must be called inside a WUOW.
     */
    virtual Timestamp getPrepareTimestamp() const {
        uasserted(ErrorCodes::CommandNotSupported,
                  "This storage engine does not support prepared transactions");
    }

    /**
     * MongoDB must update documents with non-decreasing timestamp values. A storage engine is
     * allowed to assert when this contract is violated. An untimestamped write is a subset of these
     * violations, which may be necessary in limited circumstances. This API can be called before a
     * WriteUnitOfWork begins and will suppress this subset of errors until the WriteUnitOfWork is
     * committed or rolled-back.
     */
    virtual void allowOneUntimestampedWrite() {}

    /**
     * Suppresses untimestamped write errors over the lifetime of a RecoveryUnit.
     *
     * NOTE: we should be extremely intentional with suppressing untimestamped errors. In most
     * cases we should enforce untimestamped write errors.
     */
    virtual void allowAllUntimestampedWrites() {}

    /**
     * Computes the storage level statistics accrued since the last call to this function, or
     * since the recovery unit was instantiated. Should be called at the end of each operation.
     */
    virtual std::unique_ptr<StorageStats> computeOperationStatisticsSinceLastCall() {
        return (nullptr);
    }

    /**
     * The ReadSource indicates which external or provided timestamp to read from for future
     * transactions.
     */
    enum ReadSource {
        /**
         * Read without a timestamp. This is the default.
         */
        kNoTimestamp,
        /**
         * Read from the majority all-committed timestamp.
         */
        kMajorityCommitted,
        /**
         * Read from the latest timestamp where no future transactions will commit at or before.
         */
        kNoOverlap,
        /**
         * Read from the lastApplied timestamp.
         */
        kLastApplied,
        /**
         * Read from the all_durable timestamp. New transactions will always read from the same
         * timestamp and never advance.
         */
        kAllDurableSnapshot,
        /**
         * Read from the timestamp provided to setTimestampReadSource.
         */
        kProvided,
    };

    static std::string toString(ReadSource rs) {
        switch (rs) {
            case ReadSource::kNoTimestamp:
                return "kNoTimestamp";
            case ReadSource::kMajorityCommitted:
                return "kMajorityCommitted";
            case ReadSource::kNoOverlap:
                return "kNoOverlap";
            case ReadSource::kLastApplied:
                return "kLastApplied";
            case ReadSource::kAllDurableSnapshot:
                return "kAllDurableSnapshot";
            case ReadSource::kProvided:
                return "kProvided";
        }
        MONGO_UNREACHABLE;
    }

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
        return ReadSource::kNoTimestamp;
    };

    virtual boost::optional<int64_t> getOplogVisibilityTs() {
        return boost::none;
    }
    virtual void setOplogVisibilityTs(boost::optional<int64_t> oplogVisibilityTs) {}

    /**
     * Pinning informs callers not to change the ReadSource on this RecoveryUnit. Callers are
     * expected to first check isReadSourcePinned before attempting to change the ReadSource. An
     * error may occur otherwise.
     * See also `PinReadSourceBlock` for a RAII-style solution.
     */
    virtual void pinReadSource() {}
    virtual void unpinReadSource() {}
    virtual bool isReadSourcePinned() const {
        return false;
    }

    /**
     * Sets whether this operation intends to perform reads that do not need to keep data in the
     * storage engine cache. This can be useful for operations that do large, one-time scans of
     * data, and will attempt to keep higher-priority data from being evicted from the cache. This
     * may not be called in an active transaction.
     */
    virtual void setReadOnce(bool readOnce) {};

    virtual bool getReadOnce() const {
        return false;
    };

    /**
     * Indicates whether the RecoveryUnit has an open snapshot. A snapshot can be opened inside or
     * outside of a WriteUnitOfWork.
     */
    virtual bool isActive() const {
        return _isActive();
    };

    /**
     * When called, the WriteUnitOfWork ignores the multi timestamp constraint for the remainder of
     * the WriteUnitOfWork, where if within a WriteUnitOfWork multiple timestamps are set, the first
     * timestamp must be set prior to any writes.
     *
     * Must be reset when the WriteUnitOfWork is either committed or rolled back.
     */
    virtual void ignoreAllMultiTimestampConstraints() {}

    /**
     * This function must be called when a write operation is performed on the active transaction
     * for the first time.
     *
     * Must be reset when the active transaction is either committed or rolled back.
     */
    virtual void setTxnModified() {}

    /**
     * Sets the isolation to use for subsequent operations. The isolation cannot be changed if the
     * recovery unit is active.
     */
    void setIsolation(Isolation);

    /**
     * Registers a callback to be called prior to a WriteUnitOfWork committing the storage
     * transaction. This callback may throw a WriteConflictException which will abort the
     * transaction.
     */
    virtual void registerPreCommitHook(std::function<void(OperationContext*)> callback);

    virtual void runPreCommitHooks(OperationContext* opCtx);

    /**
     * A Change is an action that is registerChange()'d while a WriteUnitOfWork exists. The
     * change is either rollback()'d or commit()'d when the WriteUnitOfWork goes out of scope.
     *
     * Neither rollback() nor commit() may fail or throw exceptions. Acquiring locks or blocking
     * operations should not be performed in these handlers, as it may lead to deadlocks.
     * LockManager locks are still held due to 2PL.
     *
     * Change implementors are responsible for handling their own synchronization, and must be aware
     * that rollback() and commit() may be called out of line and after the WriteUnitOfWork have
     * been freed. Pointers or references to stack variables should not be bound to the definitions
     * of rollback() or commit(). Each registered change will be committed or rolled back once.
     *
     * commit() handlers are passed the timestamp at which the transaction is committed. If the
     * transaction is not committed at a particular timestamp, or if the storage engine does not
     * support timestamps, then boost::none will be supplied for this parameter.
     *
     * The OperationContext provided in commit() and rollback() handlers is the current
     * OperationContext and may not be the same as when the Change was registered on the
     * RecoveryUnit. See above for usage restrictions.
     */
    class Change {
    public:
        virtual ~Change() {}

        virtual void rollback(OperationContext* opCtx) noexcept = 0;
        virtual void commit(OperationContext* opCtx,
                            boost::optional<Timestamp> commitTime) noexcept = 0;
    };

    /**
     * The commitUnitOfWork() method calls the commit() method of each registered change in order of
     * registration. The endUnitOfWork() method calls the rollback() method of each registered
     * Change in reverse order of registration. Either will unregister and delete the changes.
     *
     * The registerChange() method may only be called when a WriteUnitOfWork is active, and
     * may not be called during commit or rollback.
     */
    void registerChange(std::unique_ptr<Change> change);

    /**
     * Registers a change with the given rollback and commit functions.
     *
     * Be careful about the lifetimes of all variables captured by the callback!
     */
    template <typename RollbackCallback, typename CommitCallback>
    void registerChange(CommitCallback commit, RollbackCallback rollback) {
        class CallbackChange final : public Change {
        public:
            CallbackChange(CommitCallback&& commit, RollbackCallback&& rollback)
                : _rollback(std::move(rollback)), _commit(std::move(commit)) {}
            void rollback(OperationContext* opCtx) noexcept final {
                _rollback(opCtx);
            }
            void commit(OperationContext* opCtx, boost::optional<Timestamp> ts) noexcept final {
                _commit(opCtx, ts);
            }

        private:
            RollbackCallback _rollback;
            CommitCallback _commit;
        };

        registerChange(std::make_unique<CallbackChange>(std::move(commit), std::move(rollback)));
    }

    /**
     * Like registerChange() above but should only be used to make new state visible in the
     * in-memory catalog. Change registered with this function will commit before the commit
     * changes registered with registerChange and rollback will run after the rollback changes
     * registered with registerChange. Only one change of this kind should be registered at a given
     * time to ensure catalog updates are atomic, however multiple callbacks are allowed for testing
     * purposes.
     *
     * This separation ensures that regular Changes can observe changes to catalog visibility.
     */
    void registerChangeForCatalogVisibility(std::unique_ptr<Change> change);

    /**
     * Like registerChange() above but should only be used to push idents for two phase drop to the
     * reaper. This currently needs to happen before a drop is made visible in the catalog to avoid
     * a window where a reader would observe the drop in the catalog but not be able to find the
     * ident in the reaper.
     *
     * TODO SERVER-77959: Remove this.
     */
    void registerChangeForTwoPhaseDrop(std::unique_ptr<Change> change);

    /**
     * Registers a callback to be called if the current WriteUnitOfWork rolls back.
     *
     * Be careful about the lifetimes of all variables captured by the callback!

     * Do not capture OperationContext in this callback because it is not guaranteed to be the same
     * OperationContext to roll-back this unit of work. Use the OperationContext provided by the
     * callback instead.
     */
    template <typename Callback>
    void onRollback(Callback callback) {
        class OnRollbackChange final : public Change {
        public:
            OnRollbackChange(Callback&& callback) : _callback(std::move(callback)) {}
            void rollback(OperationContext* opCtx) noexcept final {
                _callback(opCtx);
            }
            void commit(OperationContext* opCtx, boost::optional<Timestamp>) noexcept final {}

        private:
            Callback _callback;
        };

        registerChange(std::make_unique<OnRollbackChange>(std::move(callback)));
    }

    /**
     * Registers a callback to be called if the current WriteUnitOfWork commits.
     *
     * Be careful about the lifetimes of all variables captured by the callback!
     *
     * Do not capture OperationContext in this callback because it is not guaranteed to be the same
     * OperationContext to commit this unit of work. Use the OperationContext provided by the
     * callback instead.
     */
    template <typename Callback>
    void onCommit(Callback callback) {
        class OnCommitChange final : public Change {
        public:
            OnCommitChange(Callback&& callback) : _callback(std::move(callback)) {}
            void rollback(OperationContext* opCtx) noexcept final {}
            void commit(OperationContext* opCtx,
                        boost::optional<Timestamp> commitTime) noexcept final {
                _callback(opCtx, commitTime);
            }

        private:
            Callback _callback;
        };

        registerChange(std::make_unique<OnCommitChange>(std::move(callback)));
    }

    /**
     * Registers a callback to be called if the current WriteUnitOfWork commits for two phase drop.
     *
     * Should only be used for adding drop pending idents to the reaper!
     *
     * TODO SERVER-77959: Remove this.
     */
    template <typename Callback>
    void onCommitForTwoPhaseDrop(Callback callback) {
        class OnCommitTwoPhaseChange final : public Change {
        public:
            OnCommitTwoPhaseChange(Callback&& callback) : _callback(std::move(callback)) {}
            void rollback(OperationContext* opCtx) noexcept final {}
            void commit(OperationContext* opCtx,
                        boost::optional<Timestamp> commitTime) noexcept final {
                _callback(opCtx, commitTime);
            }

        private:
            Callback _callback;
        };

        registerChangeForTwoPhaseDrop(
            std::make_unique<OnCommitTwoPhaseChange>(std::move(callback)));
    }

    virtual void setOrderedCommit(bool orderedCommit) = 0;

    /**
     * State transitions:
     *
     *   /------------------------> Inactive <-----------------------------\
     *   |                             |                                   |
     *   |                             |                                   |
     *   |              /--------------+--------------\                    |
     *   |              |                             |                    | abandonSnapshot()
     *   |              |                             |                    |
     *   |   beginUOW() |                             | _txnOpen()         |
     *   |              |                             |                    |
     *   |              V                             V                    |
     *   |    InactiveInUnitOfWork          ActiveNotInUnitOfWork ---------/
     *   |              |                             |
     *   |              |                             |
     *   |   _txnOpen() |                             | beginUOW()
     *   |              |                             |
     *   |              \--------------+--------------/
     *   |                             |
     *   |                             |
     *   |                             V
     *   |                           Active
     *   |                             |
     *   |                             |
     *   |              /--------------+--------------\
     *   |              |                             |
     *   |              |                             |
     *   |   abortUOW() |                             | commitUOW()
     *   |              |                             |
     *   |              V                             V
     *   |          Aborting                      Committing
     *   |              |                             |
     *   |              |                             |
     *   |              |                             |
     *   \--------------+-----------------------------/
     *
     */
    enum class State {
        kInactive,
        kInactiveInUnitOfWork,
        kActiveNotInUnitOfWork,
        kActive,
        kAborting,
        kCommitting,
    };

    static std::string toString(State state) {
        switch (state) {
            case State::kInactive:
                return "Inactive";
            case State::kInactiveInUnitOfWork:
                return "InactiveInUnitOfWork";
            case State::kActiveNotInUnitOfWork:
                return "ActiveNotInUnitOfWork";
            case State::kActive:
                return "Active";
            case State::kCommitting:
                return "Committing";
            case State::kAborting:
                return "Aborting";
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Exposed for debugging purposes.
     */
    State getState() const {
        return _getState();
    }

    void setNoEvictionAfterCommitOrRollback() {
        _noEvictionAfterCommitOrRollback = true;
    }

    bool getNoEvictionAfterCommitOrRollback() const {
        return _noEvictionAfterCommitOrRollback;
    }

    void setDataCorruptionDetectionMode(DataCorruptionDetectionMode mode) {
        _dataCorruptionDetectionMode = mode;
    }

    DataCorruptionDetectionMode getDataCorruptionDetectionMode() const {
        return _dataCorruptionDetectionMode;
    }

    /**
     * Returns true if this is an instance of RecoveryUnitNoop.
     */
    virtual bool isNoop() const {
        return false;
    }

    /**
     * Sets a maximum timeout that the storage engine will block an operation when the cache is
     * under pressure.
     * If not set (default 0) then the storage engine will block indefinitely.
     */
    virtual void setCacheMaxWaitTimeout(Milliseconds) {}

    /**
     * Determine the amount of cache memory this recovery unit has dirtied. If this information is
     * not perfectly accurate, prefer to return a lower bound.
     */
    virtual size_t getCacheDirtyBytes() {
        return 0;
    }

    /**
     * Returns true if currently managed by a WriteUnitOfWork.
     *
     * TODO: might should be removed after SERVER-90704
     */
    bool inUnitOfWork() const {
        return _inUnitOfWork();
    }

    /**
     * Allows callers to indicate when the operation using a RecoveryUnit is holding an exclusive
     * resource and is not allowed to block indefinitely. If an operation would block, like on a
     * prepare conflict, a StorageUnavailable exception is thrown.
     */
    void setBlockingAllowed(bool canBlock) {
        _blockingAllowed = canBlock;
    }

    /**
     * Returns true if this operation is allowed to block indefinitely for storage engine resources.
     */
    bool getBlockingAllowed() const {
        return _blockingAllowed;
    }

    bool shouldGatherWriteContextForDebugging() const {
        return _gatherWriteContextForDebugging;
    }

    void storeWriteContextForDebugging(const BSONObj& info) {
        _writeContextForDebugging.push_back(info);
    }

protected:
    RecoveryUnit() = default;

    /**
     * Returns the current state.
     */
    State _getState() const {
        return _state;
    }

    /**
     * Transitions to new state.
     *
     * Invokes openSnapshot() for all registered snapshot changes when transitioning to kActive or
     * kActiveNotInUnitOfWork from an inactive state.
     */
    void _setState(State newState);

    /**
     * Returns true if active.
     */
    bool _isActive() const {
        return State::kActiveNotInUnitOfWork == _state || State::kActive == _state;
    }

    /**
     * Returns true if currently managed by a WriteUnitOfWork.
     */
    bool _inUnitOfWork() const {
        return State::kInactiveInUnitOfWork == _state || State::kActive == _state;
    }

    /**
     * Returns true if currently running commit or rollback handlers
     */
    bool _isCommittingOrAborting() const {
        return State::kCommitting == _state || State::kAborting == _state;
    }

    /**
     * Executes all registered commit handlers and clears all registered changes
     */
    void _executeCommitHandlers(boost::optional<Timestamp> commitTimestamp);

    /**
     * Executes all registered rollback handlers and clears all registered changes
     */
    void _executeRollbackHandlers();

    bool _noEvictionAfterCommitOrRollback = false;

    AbandonSnapshotMode _abandonSnapshotMode = AbandonSnapshotMode::kAbort;

    DataCorruptionDetectionMode _dataCorruptionDetectionMode = DataCorruptionDetectionMode::kThrow;

    /**
     * Creates a new globally-unique Snapshot, if one does not exst.
     */
    void ensureSnapshot();

    /**
     * Destructs the current snapshot.
     */
    void resetSnapshot() {
        _snapshot.reset();
    }

    void setGatherWriteContextForDebugging(bool value) {
        _gatherWriteContextForDebugging = value;
    }

    std::vector<BSONObj>& getWriteContextForDebugging() {
        return _writeContextForDebugging;
    }

    OperationContext* _opCtx{nullptr};
    Isolation _isolation{Isolation::snapshot};

    bool _gatherWriteContextForDebugging{false};
    std::vector<BSONObj> _writeContextForDebugging;

private:
    virtual void doBeginUnitOfWork() = 0;
    virtual void doAbandonSnapshot() = 0;
    virtual void doCommitUnitOfWork() = 0;
    virtual void doAbortUnitOfWork() = 0;

    virtual void _setIsolation(Isolation) = 0;

    virtual void validateInUnitOfWork() const;

    std::vector<std::function<void(OperationContext*)>> _preCommitHooks;

    typedef std::vector<std::unique_ptr<Change>> Changes;
    Changes _changes;
    Changes _changesForCatalogVisibility;
    Changes _changesForTwoPhaseDrop;
    // Is constructed lazily when accessed or when an underlying storage snapshot is opened.
    boost::optional<Snapshot> _snapshot;
    State _state{State::kInactive};
    bool _readOnly{false};
    bool _blockingAllowed{true};
};

/**
 * RAII-style class to manage pinning and unpinning the readSource.
 */
class PinReadSourceBlock {
    PinReadSourceBlock(const PinReadSourceBlock&) = delete;
    PinReadSourceBlock& operator=(const PinReadSourceBlock&) = delete;

public:
    explicit PinReadSourceBlock(RecoveryUnit* recoveryUnit) : _recoveryUnit(recoveryUnit) {
        _recoveryUnit->pinReadSource();
    }

    ~PinReadSourceBlock() {
        _recoveryUnit->unpinReadSource();
    }

private:
    RecoveryUnit* const _recoveryUnit;
};


/**
 * RAII-style class to override the oplog visible timestamp of the WiredTigerRecoveryUnit while it's
 * in scope.
 */
class ScopedOplogVisibleTimestamp {
public:
    ScopedOplogVisibleTimestamp(RecoveryUnit* recoveryUnit, boost::optional<int64_t> oplogVisibleTs)
        : _recoveryUnit(recoveryUnit),
          _originalOplogVisibleTs(recoveryUnit->getOplogVisibilityTs()) {
        _recoveryUnit->setOplogVisibilityTs(oplogVisibleTs);
    }

    ScopedOplogVisibleTimestamp(const ScopedOplogVisibleTimestamp&) = delete;
    ScopedOplogVisibleTimestamp& operator=(const ScopedOplogVisibleTimestamp&) = delete;
    ScopedOplogVisibleTimestamp(ScopedOplogVisibleTimestamp&&) = delete;
    ScopedOplogVisibleTimestamp& operator=(ScopedOplogVisibleTimestamp&&) = delete;

    ~ScopedOplogVisibleTimestamp() {
        _recoveryUnit->setOplogVisibilityTs(_originalOplogVisibleTs);
    }

private:
    RecoveryUnit* _recoveryUnit;
    boost::optional<int64_t> _originalOplogVisibleTs;
};


class StorageWriteTransaction {
public:
    StorageWriteTransaction(RecoveryUnit&, bool readOnly = false);
    ~StorageWriteTransaction();

    void prepare();
    void commit();
    void abort();

private:
    RecoveryUnit& _ru;
    bool _aborted = false;
    bool _committed = false;
    bool _prepared = false;
};


}  // namespace mongo
