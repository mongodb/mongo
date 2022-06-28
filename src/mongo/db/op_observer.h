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

#include <string>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/rollback.h"

namespace mongo {

struct InsertStatement;
class OperationContext;

namespace repl {
class OpTime;
}  // namespace repl

enum class RetryableFindAndModifyLocation {
    // The operation is not retryable, or not a "findAndModify" command. Do not record a
    // pre-image.
    kNone,

    // Store the pre-image in the side collection.
    kSideCollection,

    // Store the pre-image in the oplog.
    kOplog,
};

/**
 * Holds document update information used in logging.
 */
struct OplogUpdateEntryArgs {
    CollectionUpdateArgs* updateArgs;

    NamespaceString nss;
    UUID uuid;

    // Specifies the pre-image recording option for retryable "findAndModify" commands.
    RetryableFindAndModifyLocation retryableFindAndModifyLocation =
        RetryableFindAndModifyLocation::kNone;

    OplogUpdateEntryArgs(CollectionUpdateArgs* updateArgs, NamespaceString nss, UUID uuid)
        : updateArgs(updateArgs), nss(std::move(nss)), uuid(std::move(uuid)) {}
};

struct OplogDeleteEntryArgs {
    const BSONObj* deletedDoc = nullptr;

    // "fromMigrate" indicates whether the delete was induced by a chunk migration, and so
    // should be ignored by the user as an internal maintenance operation and not a real delete.
    bool fromMigrate = false;
    bool preImageRecordingEnabledForCollection = false;
    bool changeStreamPreAndPostImagesEnabledForCollection = false;

    // Specifies the pre-image recording option for retryable "findAndModify" commands.
    RetryableFindAndModifyLocation retryableFindAndModifyLocation =
        RetryableFindAndModifyLocation::kNone;

    // Set if OpTime were reserved for the delete ahead of time.
    std::vector<OplogSlot> oplogSlots;
};

struct IndexCollModInfo {
    boost::optional<Seconds> expireAfterSeconds;
    boost::optional<Seconds> oldExpireAfterSeconds;
    boost::optional<bool> hidden;
    boost::optional<bool> oldHidden;
    boost::optional<bool> unique;
    boost::optional<bool> prepareUnique;
    boost::optional<bool> oldPrepareUnique;
    boost::optional<bool> forceNonUnique;
    std::string indexName;
};

/**
 * The OpObserver interface contains methods that get called on certain database events. It provides
 * a way for various server subsystems to be notified of other events throughout the server.
 *
 * In order to call any OpObserver method, you must be in a 'WriteUnitOfWork'. This means that any
 * locks acquired for writes in that WUOW are still held. So, you can assume that any locks required
 * to perform the operation being observed are still held. These rules should apply for all observer
 * methods unless otherwise specified.
 */
class OpObserver {
public:
    enum class CollectionDropType {
        // The collection is being dropped immediately, in one step.
        kOnePhase,

        // The collection is being dropped in two phases, by renaming to a drop pending collection
        // which is registered to be reaped later.
        kTwoPhase,
    };

    virtual ~OpObserver() = default;

    virtual void onCreateIndex(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               BSONObj indexDoc,
                               bool fromMigrate) = 0;

    virtual void onStartIndexBuild(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& collUUID,
                                   const UUID& indexBuildUUID,
                                   const std::vector<BSONObj>& indexes,
                                   bool fromMigrate) = 0;

    virtual void onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                              const NamespaceString& nss) = 0;

    /**
     * Generates a timestamp by writing a no-op oplog entry. This is only necessary for tenant
     * migrations that are aborting single-phase index builds.
     */
    virtual void onAbortIndexBuildSinglePhase(OperationContext* opCtx,
                                              const NamespaceString& nss) = 0;

    virtual void onCommitIndexBuild(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const UUID& collUUID,
                                    const UUID& indexBuildUUID,
                                    const std::vector<BSONObj>& indexes,
                                    bool fromMigrate) = 0;

    virtual void onAbortIndexBuild(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& collUUID,
                                   const UUID& indexBuildUUID,
                                   const std::vector<BSONObj>& indexes,
                                   const Status& cause,
                                   bool fromMigrate) = 0;

    virtual void onInserts(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& uuid,
                           std::vector<InsertStatement>::const_iterator begin,
                           std::vector<InsertStatement>::const_iterator end,
                           bool fromMigrate) = 0;
    virtual void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) = 0;
    virtual void aboutToDelete(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               const BSONObj& doc) = 0;

    /**
     * Handles logging before document is deleted.
     *
     * "ns" name of the collection from which deleteState.idDoc will be deleted.
     *
     * "args" is a reference to information detailing whether the pre-image of the doc should be
     * preserved with deletion. If `retryableWritePreImageRecordingType != kNotRetryable`, then the
     * opObserver must store the `deletedDoc` in addition to the documentKey.
     */
    virtual void onDelete(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const UUID& uuid,
                          StmtId stmtId,
                          const OplogDeleteEntryArgs& args) = 0;

    /**
     * Logs a no-op with "msgObj" in the o field into oplog.
     *
     * This function should only be used internally. "nss", "uuid", "o2", and the opTimes should
     * never be exposed to users (for instance through the appendOplogNote command).
     */
    virtual void onInternalOpMessage(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const boost::optional<UUID>& uuid,
                                     const BSONObj& msgObj,
                                     boost::optional<BSONObj> o2MsgObj,
                                     boost::optional<repl::OpTime> preImageOpTime,
                                     boost::optional<repl::OpTime> postImageOpTime,
                                     boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                                     boost::optional<OplogSlot> slot) = 0;

    /**
     * Logs a no-op with "msgObj" in the o field into oplog.
     */
    void onOpMessage(OperationContext* opCtx, const BSONObj& msgObj) {
        onInternalOpMessage(opCtx,
                            {},
                            boost::none,
                            msgObj,
                            boost::none,
                            boost::none,
                            boost::none,
                            boost::none,
                            boost::none);
    }

    virtual void onCreateCollection(OperationContext* opCtx,
                                    const CollectionPtr& coll,
                                    const NamespaceString& collectionName,
                                    const CollectionOptions& options,
                                    const BSONObj& idIndex,
                                    const OplogSlot& createOpTime,
                                    bool fromMigrate) = 0;
    /**
     * This function logs an oplog entry when a 'collMod' command on a collection is executed.
     * Since 'collMod' commands can take a variety of different formats, the 'o' field of the
     * oplog entry is populated with the 'collMod' command object. For TTL index updates, we
     * transform key pattern index specifications into index name specifications, for uniformity.
     * All other collMod fields are added to the 'o' object without modifications.
     *
     * To facilitate the rollback process, 'oldCollOptions' contains the previous state of all
     * collection options i.e. the state prior to completion of the current collMod command.
     * 'ttlInfo' contains the index name and previous expiration time of a TTL index. The old
     * collection options will be stored in the 'o2.collectionOptions_old' field, and the old TTL
     * expiration value in the 'o2.expireAfterSeconds_old' field.
     *
     * Oplog Entry Example ('o' and 'o2' fields shown):
     *
     *      {
     *          ...
     *          o: {
     *              collMod: "test",
     *              validationLevel: "off",
     *              index: {name: "indexName_1", expireAfterSeconds: 600}
     *          }
     *          o2: {
     *              collectionOptions_old: {
     *                  validationLevel: "strict",
     *              },
     *              expireAfterSeconds_old: 300
     *          }
     *      }
     *
     */
    virtual void onCollMod(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& uuid,
                           const BSONObj& collModCmd,
                           const CollectionOptions& oldCollOptions,
                           boost::optional<IndexCollModInfo> indexInfo) = 0;
    virtual void onDropDatabase(OperationContext* opCtx, const std::string& dbName) = 0;

    /**
     * This function logs an oplog entry when a 'drop' command on a collection is executed.
     * Returns the optime of the oplog entry successfully written to the oplog.
     * Returns a null optime if an oplog entry was not written for this operation.
     *
     * 'dropType' describes whether the collection drop is one-phase or two-phase.
     */
    virtual repl::OpTime onDropCollection(OperationContext* opCtx,
                                          const NamespaceString& collectionName,
                                          const UUID& uuid,
                                          std::uint64_t numRecords,
                                          CollectionDropType dropType) = 0;
    virtual repl::OpTime onDropCollection(OperationContext* opCtx,
                                          const NamespaceString& collectionName,
                                          const UUID& uuid,
                                          std::uint64_t numRecords,
                                          CollectionDropType dropType,
                                          bool markFromMigrate) {
        return onDropCollection(opCtx, collectionName, uuid, numRecords, dropType);
    }


    /**
     * This function logs an oplog entry when an index is dropped. The namespace of the index,
     * the index name, and the index info from the index descriptor are used to create a
     * 'dropIndexes' op where the 'o' field is the name of the index and the 'o2' field is the
     * index info. The index info can then be used to reconstruct the index on rollback.
     *
     * If a user specifies {dropIndexes: 'foo', index: '*'}, each index dropped will have its own
     * oplog entry. This means it's possible to roll back half of the index drops.
     */
    virtual void onDropIndex(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const UUID& uuid,
                             const std::string& indexName,
                             const BSONObj& indexInfo) = 0;

    /**
     * This function logs an oplog entry when a 'renameCollection' command on a collection is
     * executed. It should be used specifically in instances where the optime is necessary to
     * be obtained prior to performing the actual rename, and should only be used in conjunction
     * with postRenameCollection.
     * Returns the optime of the oplog entry successfully written to the oplog.
     * Returns a null optime if an oplog entry was not written for this operation.
     */
    virtual repl::OpTime preRenameCollection(OperationContext* opCtx,
                                             const NamespaceString& fromCollection,
                                             const NamespaceString& toCollection,
                                             const UUID& uuid,
                                             const boost::optional<UUID>& dropTargetUUID,
                                             std::uint64_t numRecords,
                                             bool stayTemp) = 0;
    virtual repl::OpTime preRenameCollection(OperationContext* opCtx,
                                             const NamespaceString& fromCollection,
                                             const NamespaceString& toCollection,
                                             const UUID& uuid,
                                             const boost::optional<UUID>& dropTargetUUID,
                                             std::uint64_t numRecords,
                                             bool stayTemp,
                                             bool markFromMigrate) {
        return preRenameCollection(
            opCtx, fromCollection, toCollection, uuid, dropTargetUUID, numRecords, stayTemp);
    }
    /**
     * This function performs all op observer handling for a 'renameCollection' command except for
     * logging the oplog entry. It should be used specifically in instances where the optime is
     * necessary to be obtained prior to performing the actual rename, and should only be used in
     * conjunction with preRenameCollection.
     */
    virtual void postRenameCollection(OperationContext* opCtx,
                                      const NamespaceString& fromCollection,
                                      const NamespaceString& toCollection,
                                      const UUID& uuid,
                                      const boost::optional<UUID>& dropTargetUUID,
                                      bool stayTemp) = 0;
    /**
     * This function logs an oplog entry when a 'renameCollection' command on a collection is
     * executed. It calls preRenameCollection to log the entry and postRenameCollection to do all
     * other handling.
     */
    virtual void onRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromCollection,
                                    const NamespaceString& toCollection,
                                    const UUID& uuid,
                                    const boost::optional<UUID>& dropTargetUUID,
                                    std::uint64_t numRecords,
                                    bool stayTemp) = 0;
    virtual void onRenameCollection(OperationContext* opCtx,
                                    const NamespaceString& fromCollection,
                                    const NamespaceString& toCollection,
                                    const UUID& uuid,
                                    const boost::optional<UUID>& dropTargetUUID,
                                    std::uint64_t numRecords,
                                    bool stayTemp,
                                    bool markFromMigrate) {
        onRenameCollection(
            opCtx, fromCollection, toCollection, uuid, dropTargetUUID, numRecords, stayTemp);
    }

    virtual void onImportCollection(OperationContext* opCtx,
                                    const UUID& importUUID,
                                    const NamespaceString& nss,
                                    long long numRecords,
                                    long long dataSize,
                                    const BSONObj& catalogEntry,
                                    const BSONObj& storageMetadata,
                                    bool isDryRun) = 0;

    virtual void onApplyOps(OperationContext* opCtx,
                            const std::string& dbName,
                            const BSONObj& applyOpCmd) = 0;
    virtual void onEmptyCapped(OperationContext* opCtx,
                               const NamespaceString& collectionName,
                               const UUID& uuid) = 0;

    /**
     * The onUnpreparedTransactionCommit method is called on the commit of an unprepared
     * transaction, before the RecoveryUnit onCommit() is called.  It must not be called when no
     * transaction is active.
     *
     * The 'statements' are the list of CRUD operations to be applied in this transaction.
     *
     * The 'numberOfPrePostImagesToWrite' is the number of CRUD operations that have a pre-image
     * to write as a noop oplog entry. The op observer will reserve oplog slots for these
     * preimages in addition to the statements.
     */
    virtual void onUnpreparedTransactionCommit(OperationContext* opCtx,
                                               std::vector<repl::ReplOperation>* statements,
                                               size_t numberOfPrePostImagesToWrite) = 0;
    /**
     * The onPreparedTransactionCommit method is called on the commit of a prepared transaction,
     * after the RecoveryUnit onCommit() is called.  It must not be called when no transaction is
     * active.
     *
     * The 'commitOplogEntryOpTime' is passed in to be used as the OpTime of the oplog entry. The
     * 'commitTimestamp' is the timestamp at which the multi-document transaction was committed.
     *
     * The 'statements' are the list of CRUD operations to be applied in this transaction.
     */
    virtual void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept = 0;

    /**
     * Events for logical grouping of writes to be replicated atomically.
     * After onBatchedWriteStart(), the replication subsystem is prepared to
     * start collecting operations to replicate in an applyOps oplog entry.
     */
    virtual void onBatchedWriteStart(OperationContext* opCtx) = 0;

    /**
     * The write operations between onBatchedWriteStart() and onBatchedWriteCommit()
     * are gathered in a single applyOps oplog entry, similar to atomic applyOps and
     * multi-doc transactions, and written to the oplog.
     */
    virtual void onBatchedWriteCommit(OperationContext* opCtx) = 0;

    /**
     * Clears the accumulated write operations. No further writes is allowed in this storage
     * transaction (WriteUnitOfWork). Calling this function after onBatchedWriteCommit()
     * should be fine for cleanup purposes.
     */
    virtual void onBatchedWriteAbort(OperationContext* opCtx) = 0;

    /**
     * Contains "applyOps" oplog entries and oplog slots to be used for writing pre- and post- image
     * oplog entries for a transaction. "applyOps" entries are not actual "applyOps" entries to be
     * written to the oplog, but comprise certain parts of those entries - BSON serialized
     * operations, and the assigned oplog slot. The operations in field 'ApplyOpsEntry::operations'
     * should be considered opaque outside the OpObserver.
     */
    struct ApplyOpsOplogSlotAndOperationAssignment {
        struct ApplyOpsEntry {
            OplogSlot oplogSlot;
            std::vector<BSONObj> operations;
        };

        // Oplog slots to be used for writing pre- and post- image oplog entries.
        std::vector<OplogSlot> prePostImageOplogEntryOplogSlots;

        // Representation of "applyOps" oplog entries.
        std::vector<ApplyOpsEntry> applyOpsEntries;

        // Number of oplog slots utilized.
        size_t numberOfOplogSlotsUsed;
    };

    /**
     * This method is called before an atomic transaction is prepared. It must be called when a
     * transaction is active.
     *
     * Optionally returns a representation of "applyOps" entries to be written and oplog slots to be
     * used for writing pre- and post- image oplog entries for a transaction. Only one OpObserver in
     * the system should return the representation of "applyOps" entries. The returned value is
     * passed to 'onTransactionPrepare()'.
     *
     * The 'reservedSlots' is a list of oplog slots reserved for the oplog entries in a transaction.
     * The last reserved slot represents the prepareOpTime used for the prepare oplog entry.
     *
     * The 'numberOfPrePostImagesToWrite' is the number of CRUD operations that have a pre-image
     * to write as a noop oplog entry.
     *
     * The 'wallClockTime' is the time to record as wall clock time on oplog entries resulting from
     * transaction preparation.
     *
     * The 'statements' are the list of CRUD operations to be applied in this transaction. The
     * operations may be modified by setting pre-image and post-image oplog entry timestamps.
     */
    virtual std::unique_ptr<ApplyOpsOplogSlotAndOperationAssignment> preTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        std::vector<repl::ReplOperation>* statements) = 0;

    /**
     * The onTransactionPrepare method is called when an atomic transaction is prepared. It must be
     * called when a transaction is active.
     *
     * 'reservedSlots' is a list of oplog slots reserved for the oplog entries in a transaction. The
     * last reserved slot represents the prepareOpTime used for the prepare oplog entry.
     *
     * The 'statements' are the list of CRUD operations to be applied in this transaction.
     *
     * The 'applyOpsOperationAssignment' contains a representation of "applyOps" entries and oplog
     * slots to be used for writing pre- and post- image oplog entries for a transaction. A value
     * returned by 'preTransactionPrepare()' should be passed as 'applyOpsOperationAssignment'.
     *
     * The 'numberOfPrePostImagesToWrite' is the number of CRUD operations that have a pre-image
     * to write as a noop oplog entry. The op observer will reserve oplog slots for these
     * preimages in addition to the statements.
     *
     * The 'wallClockTime' is the time to record as wall clock time on oplog entries resulting from
     * transaction preparation. The same time value should be passed to 'preTransactionPrepare()'.
     */
    virtual void onTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        std::vector<repl::ReplOperation>* statements,
        const ApplyOpsOplogSlotAndOperationAssignment* applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime) = 0;

    /**
     * The onTransactionAbort method is called when an atomic transaction aborts, before the
     * RecoveryUnit onRollback() is called. It must not be called when the transaction to abort is
     * active.
     *
     * If the transaction was prepared, then 'abortOplogEntryOpTime' is passed in to be used as the
     * OpTime of the oplog entry.
     */
    virtual void onTransactionAbort(OperationContext* opCtx,
                                    boost::optional<OplogSlot> abortOplogEntryOpTime) = 0;

    /**
     * A structure to hold information about a replication rollback suitable to be passed along to
     * any external subsystems that need to be notified of a rollback occurring.
     */
    struct RollbackObserverInfo {
        // A count of all oplog entries seen during rollback (even no-op entries).
        std::uint32_t numberOfEntriesObserved;

        // Set of all namespaces from ops being rolled back.
        std::set<NamespaceString> rollbackNamespaces = {};

        // Set of all session ids from ops being rolled back.
        std::set<UUID> rollbackSessionIds = {};

        // Maps UUIDs to a set of BSONObjs containing the _ids of the documents that will be deleted
        // from that collection due to rollback, and is used to populate rollback files.
        // For simplicity, this BSONObj set uses the simple binary comparison, as it is never wrong
        // to consider two _ids as distinct even if the collection default collation would put them
        // in the same equivalence class.
        stdx::unordered_map<UUID, SimpleBSONObjUnorderedSet, UUID::Hash> rollbackDeletedIdsMap;

        // True if the shard identity document was rolled back.
        bool shardIdentityRolledBack = false;

        // True if the config.version document was rolled back.
        bool configServerConfigVersionRolledBack = false;

        // Maps command names to a count of the number of those commands that are being rolled back.
        StringMap<long long> rollbackCommandCounts;
    };

    /**
     * This function will get called after the replication system has completed a rollback. This
     * means that all on-disk, replicated data will have been reverted to the rollback common point
     * by the time this function is called. Subsystems may use this method to invalidate any in
     * memory caches or, optionally, rebuild any data structures from the data that is now on disk.
     * This function should not write any persistent state.
     *
     * When this function is called, there will be no locks held on the given OperationContext, and
     * it will not be called inside an existing WriteUnitOfWork. Any work done inside this handler
     * is expected to handle this on its own.
     *
     * This method is only applicable to the "rollback to a stable timestamp" algorithm, and is not
     * called when using any other rollback algorithm i.e "rollback via refetch".
     *
     * This function will call the private virtual '_onReplicationRollback' method. Any exceptions
     * thrown indicates rollback failure that may have led us to some inconsistent on-disk or memory
     * state, so we crash instead.
     */
    void onReplicationRollback(OperationContext* opCtx,
                               const RollbackObserverInfo& rbInfo) noexcept {
        try {
            _onReplicationRollback(opCtx, rbInfo);
        } catch (const DBException& ex) {
            fassert(6050902, ex.toStatus());
        }
    };

    /**
     * Called when the majority commit point is updated by replication.
     *
     * This is called while holding a very hot mutex (the ReplicationCoordinator mutex). Therefore
     * it should avoid doing any work that can be done later, and avoid calling back into any
     * replication functions that take this mutex (which would cause self-deadlock).
     */
    virtual void onMajorityCommitPointUpdate(ServiceContext* service,
                                             const repl::OpTime& newCommitPoint) = 0;

    struct Times;

private:
    virtual void _onReplicationRollback(OperationContext* opCtx,
                                        const RollbackObserverInfo& rbInfo) = 0;

protected:
    class ReservedTimes;
};

/**
 * This struct is a decoration for `OperationContext` which contains collected `repl::OpTime`
 * and `Date_t` timestamps of various critical stages of an operation performed by an OpObserver
 * chain.
 */
struct OpObserver::Times {
    static Times& get(OperationContext*);

    std::vector<repl::OpTime> reservedOpTimes;

private:
    friend OpObserver::ReservedTimes;

    // Because `OpObserver`s are re-entrant, it is necessary to track the recursion depth to know
    // when to actually clear the `reservedOpTimes` vector, using the `ReservedTimes` scope object.
    int _recursionDepth = 0;
};

/**
 * This class is an RAII object to manage the state of the `OpObserver::Times` decoration on an
 * operation context. Upon destruction the list of times in the decoration on the operation context
 * is cleared. It is intended for use as a scope object in `OpObserverRegistry` to manage
 * re-entrancy.
 */
class OpObserver::ReservedTimes {
    ReservedTimes(const ReservedTimes&) = delete;
    ReservedTimes& operator=(const ReservedTimes&) = delete;

public:
    explicit ReservedTimes(OperationContext* opCtx);
    ~ReservedTimes();

    const Times& get() const {
        return _times;
    }

private:
    Times& _times;
};

}  // namespace mongo
