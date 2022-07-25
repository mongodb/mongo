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

#include <cstdint>
#include <stdlib.h>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/util/decorable.h"

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
    kEnforce,

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
    kIgnoreConflictsAllowWrites
};

/**
 * Storage statistics management class, with interfaces to provide the statistics in the BSON format
 * and an operator to add the statistics values.
 */
class StorageStats {
    StorageStats(const StorageStats&) = delete;
    StorageStats& operator=(const StorageStats&) = delete;

public:
    StorageStats() = default;

    virtual ~StorageStats(){};

    /**
     * Provides the storage statistics in the form of a BSONObj.
     */
    virtual BSONObj toBSON() = 0;

    /**
     * Add the statistics values.
     */
    virtual StorageStats& operator+=(const StorageStats&) = 0;

    /**
     * Provides the ability to create an instance of this class outside of the storage integration
     * layer.
     */
    virtual std::shared_ptr<StorageStats> getCopy() = 0;
};


/**
 * A RecoveryUnit is responsible for ensuring that data is persisted.
 * All on-disk information must be mutated through this interface.
 */
class RecoveryUnit : public Decorable<RecoveryUnit> {
    RecoveryUnit(const RecoveryUnit&) = delete;
    RecoveryUnit& operator=(const RecoveryUnit&) = delete;

public:
    // Behavior for abandonSnapshot().
    enum class AbandonSnapshotMode {
        kAbort,  // default
        kCommit
    };

    void commitRegisteredChanges(boost::optional<Timestamp> commitTimestamp);
    void abortRegisteredChanges();
    virtual ~RecoveryUnit() {}

    /**
     * Marks the beginning of a unit of work. Each call must be matched with exactly one call to
     * either commitUnitOfWork or abortUnitOfWork.
     *
     * When called with readOnly=true, no unit of work is started. Calling commitUnitOfWork or
     * abortUnitOfWork will invariant.
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
     * Cleans up any state set for this unit of work.
     *
     * Should be called through WriteUnitOfWork rather than directly.
     */
    void endReadOnlyUnitOfWork();

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
     * Dictates whether to round up prepare and commit timestamp of a prepared transaction. If set
     * to true, the prepare timestamp will be rounded up to the oldest timestamp if found to be
     * earlier; and the commit timestamp will be rounded up to the prepare timestamp if found to
     * be earlier.
     *
     * This must be called before a transaction begins, and defaults to false. On transaction close,
     * we reset the value to its default.
     *
     */
    virtual void setRoundUpPreparedTimestamps(bool value) {}

    /**
     * Waits until all commits that happened before this call are durable in the journal. Returns
     * true, unless the storage engine cannot guarantee durability, which should never happen when
     * the engine is non-ephemeral. This cannot be called from inside a unit of work, and should
     * fail if it is. This method invariants if the caller holds any locks, except for repair.
     *
     * Can throw write interruption errors from the JournalListener.
     */
    virtual bool waitUntilDurable(OperationContext* opCtx) = 0;

    /**
     * Unlike `waitUntilDurable`, this method takes a stable checkpoint, making durable any writes
     * on unjournaled tables that are behind the current stable timestamp. If the storage engine
     * is starting from an "unstable" checkpoint or 'stableCheckpoint'=false, this method call will
     * turn into an unstable checkpoint.
     *
     * This must not be called by a system taking user writes until after a stable timestamp is
     * passed to the storage engine.
     */
    virtual bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx, bool stableCheckpoint) {
        return waitUntilDurable(opCtx);
    }

    /**
     * If there is an open transaction, it is closed. If the current AbandonSnapshotMode is
     * 'kAbort', the transaction is aborted. If the mode is 'kCommit' the transaction is committed,
     * and all data currently pointed to by cursors remains pinned until the cursors are
     * repositioned.

     * On return no transaction is active. It is a programming error to call this inside of a
     * WriteUnitOfWork, even if the AbandonSnapshotMode is 'kCommit'.
     */
    void abandonSnapshot() {
        doAbandonSnapshot();
        assignNextSnapshotId();
    }

    void setAbandonSnapshotMode(AbandonSnapshotMode mode) {
        _abandonSnapshotMode = mode;
    }
    AbandonSnapshotMode abandonSnapshotMode() const {
        return _abandonSnapshotMode;
    }

    /**
     * Informs the RecoveryUnit that a snapshot will be needed soon, if one was not already
     * established. This specifically allows the storage engine to preallocate any required
     * transaction resources while minimizing the critical section between generating a new
     * timestamp and setting it using setTimestamp.
     */
    virtual void preallocateSnapshot() {}

    /**
     * Like preallocateSnapshot() above but also indicates that the snapshot will be used for
     * reading the oplog.
     *
     * StorageEngines may not implement this in which case it works like preallocateSnapshot.
     */
    virtual void preallocateSnapshotForOplogRead() {
        preallocateSnapshot();
    }

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
    virtual boost::optional<Timestamp> getPointInTimeReadTimestamp(OperationContext* opCtx) {
        return boost::none;
    }

    /**
     * Gets the local SnapshotId.
     *
     * It is only valid to compare SnapshotIds generated by a single RecoveryUnit.
     *
     * This is unrelated to Timestamp which must be globally comparable.
     */
    SnapshotId getSnapshotId() const {
        return SnapshotId{_mySnapshotId};
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
     * Sets catalog conflicting timestamp.
     * This cannot be called after WiredTigerRecoveryUnit::_txnOpen.
     *
     * This value must be set when both of the following conditions are true:
     * - A storage engine snapshot is opened without a read timestamp
     * (RecoveryUnit::ReadSource::kNoTimestamp).
     * - The transaction may touch collections it does not yet have locks for.
     * In this circumstance, the catalog conflicting timestamp serves as a substitute for a read
     * timestamp. This value must be set to a valid (i.e: no-holes) read timestamp prior to
     * acquiring a storage engine snapshot. This timestamp will be used to determine if any changes
     * had happened to the in-memory catalog after a storage engine snapshot got opened for that
     * transaction.
     */
    virtual void setCatalogConflictingTimestamp(Timestamp timestamp) {}

    /**
     * Returns the catalog conflicting timestamp.
     */
    virtual Timestamp getCatalogConflictingTimestamp() const {
        return {};
    }

    /**
     * MongoDB must update documents with non-decreasing timestamp values. A storage engine is
     * allowed to assert when this contract is violated. An untimestamped write is a subset of these
     * violations, which may be necessary in limited circumstances. This API can be called before a
     * transaction begins to suppress this subset of errors.
     */
    virtual void allowUntimestampedWrite() {}

    /**
     * Fetches the storage level statistics.
     */
    virtual std::shared_ptr<StorageStats> getOperationStatistics() const {
        return (nullptr);
    }

    /**
     * Refreshes a read transaction by resetting the snapshot in use
     */
    virtual void refreshSnapshot() {}

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
        kProvided
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
    virtual void setReadOnce(bool readOnce){};

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
    virtual void ignoreAllMultiTimestampConstraints(){};

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
            void rollback() final {
                _rollback();
            }
            void commit(boost::optional<Timestamp> ts) final {
                _commit(ts);
            }

        private:
            RollbackCallback _rollback;
            CommitCallback _commit;
        };

        registerChange(std::make_unique<CallbackChange>(std::move(commit), std::move(rollback)));
    }

    /**
     * Like registerChange() above but should only be used to make new state visible in the
     * in-memory catalog. Only one change of this kind may be registered at a given time to ensure
     * catalog updates are atomic. Change registered with this function will commit after the commit
     * changes registered with registerChange and rollback will run before the rollback changes
     * registered with registerChange.
     *
     * This separation ensures that regular Changes that can modify state are run before the Change
     * to install the new state in the in-memory catalog, after which there should be no further
     * changes.
     */
    void registerChangeForCatalogVisibility(std::unique_ptr<Change> change);

    /**
     * Returns true if a change has been registered with registerChangeForCatalogVisibility() above.
     */
    bool hasRegisteredChangeForCatalogVisibility();

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

        registerChange(std::make_unique<OnRollbackChange>(std::move(callback)));
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

        registerChange(std::make_unique<OnCommitChange>(std::move(callback)));
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
    State getState() {
        return _getState();
    }

    void setNoEvictionAfterRollback() {
        _noEvictionAfterRollback = true;
    }

    bool getNoEvictionAfterRollback() const {
        return _noEvictionAfterRollback;
    }

    /**
     * Returns true if this is an instance of RecoveryUnitNoop.
     */
    virtual bool isNoop() const {
        return false;
    }

protected:
    RecoveryUnit();

    /**
     * Returns the current state.
     */
    State _getState() const {
        return _state;
    }

    /**
     * Transitions to new state.
     */
    void _setState(State newState) {
        _state = newState;
    }

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

    bool _noEvictionAfterRollback = false;

    AbandonSnapshotMode _abandonSnapshotMode = AbandonSnapshotMode::kAbort;

private:
    // Sets the snapshot associated with this RecoveryUnit to a new globally unique id number.
    void assignNextSnapshotId();

    virtual void doBeginUnitOfWork() = 0;
    virtual void doAbandonSnapshot() = 0;
    virtual void doCommitUnitOfWork() = 0;
    virtual void doAbortUnitOfWork() = 0;

    virtual void validateInUnitOfWork() const;

    std::vector<std::function<void(OperationContext*)>> _preCommitHooks;

    typedef std::vector<std::unique_ptr<Change>> Changes;
    Changes _changes;
    std::unique_ptr<Change> _changeForCatalogVisibility;
    State _state = State::kInactive;
    uint64_t _mySnapshotId;
    bool _readOnly = false;
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

}  // namespace mongo
