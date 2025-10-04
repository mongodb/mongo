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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/commit_quorum_options.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/transaction/transaction_operations.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace MONGO_MOD_PUB mongo {

class DocumentKey;
struct IndexBuildInfo;
struct InsertStatement;

struct OpTimeBundle {
    repl::OpTime writeOpTime;
    Date_t wallClockTime;
};

/**
 * The generic container for onUpdate/onDelete/onUnpreparedTransactionCommit state-passing between
 * OpObservers. Despite the naming, some OpObserver's don't strictly observe. This struct is written
 * by OpObserverImpl and useful for later observers to inspect state they need.
 *
 * These structs are decorable to support the sharing of critical resources between OpObserverImpl
 * and MigrationChunkClonerSourceOpObserver. No other decorations should be added to these structs.
 */
struct OpStateAccumulator : Decorable<OpStateAccumulator> {
    OpStateAccumulator() = default;

    // Use either 'opTime' for non-insert operations or 'insertOpTimes', but not both.
    OpTimeBundle opTime;
    std::vector<repl::OpTime> insertOpTimes;

    // Temporary pre/post image information for a retryable findAndModify operation to be written
    // to the image collection (config.image_collection).
    boost::optional<repl::ReplOperation::ImageBundle> retryableFindAndModifyImageToWrite;

private:
    OpStateAccumulator(const OpStateAccumulator&) = delete;
    OpStateAccumulator& operator=(const OpStateAccumulator&) = delete;
};

enum class RetryableFindAndModifyLocation {
    // The operation is not retryable, or not a "findAndModify" command. Do not record a
    // pre-image.
    kNone,

    // Store the pre-image in the side collection.
    kSideCollection,
};

/**
 * Holds document update information used in logging.
 */
struct OplogUpdateEntryArgs {
    CollectionUpdateArgs* updateArgs;

    const CollectionPtr& coll;

    // Specifies the pre-image recording option for retryable "findAndModify" commands.
    RetryableFindAndModifyLocation retryableFindAndModifyLocation =
        RetryableFindAndModifyLocation::kNone;

    OplogUpdateEntryArgs(CollectionUpdateArgs* updateArgs, const CollectionPtr& coll)
        : updateArgs(updateArgs), coll(coll) {}
};

/**
 * Holds supplementary information required for OpObserver::onDelete() to write out an
 * oplog entry for deleting a single document from a collection.
 */
struct OplogDeleteEntryArgs : Decorable<OplogDeleteEntryArgs> {
    // "fromMigrate" indicates whether the delete was induced by a chunk migration, and so
    // should be ignored by the user as an internal maintenance operation and not a real delete.
    bool fromMigrate = false;
    bool changeStreamPreAndPostImagesEnabledForCollection = false;

    // Specifies the pre-image recording option for retryable "findAndModify" commands.
    RetryableFindAndModifyLocation retryableFindAndModifyLocation =
        RetryableFindAndModifyLocation::kNone;

    // Non-null when the RecordId for the delete is / needs to be replicated.
    RecordId replicatedRecordId;

    // Set if OpTimes were reserved for the delete ahead of time for this retryable
    // "findAndModify" operation.
    // Implies 'retryableFindAndModifyLocation' is set to kSideCollection but the
    // other way round (because of multi-doc transactions).
    // See reserveOplogSlotsForRetryableFindAndModify() in collection_write_path.cpp.
    std::vector<OplogSlot> retryableFindAndModifyOplogSlots;
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
class MONGO_MOD_OPEN OpObserver {
public:
    using ApplyOpsOplogSlotAndOperationAssignment = TransactionOperations::ApplyOpsInfo;

    /**
     * Used by CRUD ops: onInserts, onUpdate, and onDelete.
     */
    enum class NamespaceFilter {
        kConfig,           // config database (i.e. config.*)
        kSystem,           // system collection (i.e. *.system.*)
        kConfigAndSystem,  // run the observer on config and system, but not user collections
        kAll,              // run the observer on all collections/databases
        kNone,             // never run the observer for this CRUD event
    };

    // Controls the OpObserverRegistry's filtering of CRUD events.
    // Each OpObserver declares which events it cares about with this.
    struct NamespaceFilters {
        NamespaceFilter updateFilter;  // onInserts, onUpdate
        NamespaceFilter deleteFilter;  // onDelete
    };

    virtual ~OpObserver() = default;

    // Used by the OpObserverRegistry to filter out CRUD operations.
    // With this method, each OpObserver should declare if it wants to subscribe
    // to a subset of operations to special internal collections. This helps
    // improve performance. Avoid using 'kAll' as much as possible.
    virtual NamespaceFilters getNamespaceFilters() const = 0;

    virtual void onCreateIndex(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const UUID& uuid,
                               const IndexBuildInfo& indexBuildInfo,
                               bool fromMigrate,
                               bool isTimeseries = false) = 0;

    virtual void onStartIndexBuild(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& collUUID,
                                   const UUID& indexBuildUUID,
                                   const std::vector<IndexBuildInfo>& indexes,
                                   bool fromMigrate,
                                   bool isTimeseries = false) = 0;

    virtual void onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                              const NamespaceString& nss) = 0;

    virtual void onCommitIndexBuild(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const UUID& collUUID,
                                    const UUID& indexBuildUUID,
                                    const std::vector<BSONObj>& indexes,
                                    bool fromMigrate,
                                    bool isTimeseries = false) = 0;

    virtual void onAbortIndexBuild(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& collUUID,
                                   const UUID& indexBuildUUID,
                                   const std::vector<BSONObj>& indexes,
                                   const Status& cause,
                                   bool fromMigrate,
                                   bool isTimeseries = false) = 0;

    /**
     * 'recordIds' is a vector of recordIds corresponding to the inserted documents.
     * The presence of a non-empty vector of recordIds indicates that the recordIds should
     * be added to the oplog.
     * 'fromMigrate' array contains settings for each insert operation and takes into
     * account orphan documents.
     * 'defaultFromMigrate' is the initial 'fromMigrate' value for the 'fromMigrate' array
     * and is intended to be forwarded to downstream subsystems that expect a single
     * 'fromMigrate' to describe the entire set of inserts.
     * Examples: ShardServerOpObserver, UserWriteBlockModeOpObserver, and
     * MigrationChunkClonerSourceOpObserver::onInserts().
     *
     * The 'defaultFromMigrate' value must be consistent with the 'fromMigrate' array; that is,
     * if 'defaultFromMigrate' is true, all entries in the 'fromMigrate' array must be true.
     */
    virtual void onInserts(OperationContext* opCtx,
                           const CollectionPtr& coll,
                           std::vector<InsertStatement>::const_iterator begin,
                           std::vector<InsertStatement>::const_iterator end,
                           const std::vector<RecordId>& recordIds,
                           std::vector<bool> fromMigrate,
                           bool defaultFromMigrate,
                           OpStateAccumulator* opAccumulator = nullptr) = 0;

    virtual void onUpdate(OperationContext* opCtx,
                          const OplogUpdateEntryArgs& args,
                          OpStateAccumulator* opAccumulator = nullptr) = 0;

    /**
     * Handles logging before document is deleted.
     *
     * "ns" name of the collection from which deleteState.idDoc will be deleted.
     *
     * "doc" holds the pre-image of the document to be deleted.
     *
     * "args" is a reference to information detailing whether the pre-image of the doc should be
     * preserved with deletion.
     */
    virtual void onDelete(OperationContext* opCtx,
                          const CollectionPtr& coll,
                          StmtId stmtId,
                          const BSONObj& doc,
                          const DocumentKey& documentKey,
                          const OplogDeleteEntryArgs& args,
                          OpStateAccumulator* opAccumulator = nullptr) = 0;

    virtual void onContainerInsert(OperationContext* opCtx,
                                   const NamespaceString& ns,
                                   const UUID& collUUID,
                                   StringData ident,
                                   int64_t key,
                                   std::span<const char> value) = 0;

    virtual void onContainerInsert(OperationContext* opCtx,
                                   const NamespaceString& ns,
                                   const UUID& collUUID,
                                   StringData ident,
                                   std::span<const char> key,
                                   std::span<const char> value) = 0;

    virtual void onContainerDelete(OperationContext* opCtx,
                                   const NamespaceString& ns,
                                   const UUID& collUUID,
                                   StringData ident,
                                   int64_t key) = 0;

    virtual void onContainerDelete(OperationContext* opCtx,
                                   const NamespaceString& ns,
                                   const UUID& collUUID,
                                   StringData ident,
                                   std::span<const char> key) = 0;

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

    /**
     * Signals to observers that a new collection has been created.
     * - 'collectionName': The namespace of the new collection.
     * - 'options': The main options for collection creation.
     * - 'idIndex': The spec for the '_id_' index automatically generated as a part of collection
     * creation. Empty if no '_id_' index was generated.
     * - 'createOpTime': The reserved timestamp for collection creation outside a multi-document
     * transaction. Unset for creation inside a multi-document transaction as multi-document
     * transactions reserve the appropriate oplog slots at commit time.
     * - 'createCollCatalogIdentifier': Information about how the collection was registered in the
     * local catalog and storage engine. 'boost::none' if the collection was not persisted to the
     * local catalog.
     * - 'fromMigrate': Whether collection creation was driven by a migration.
     */
    virtual void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
        bool fromMigrate,
        bool isTimeseries = false) = 0;

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
                           boost::optional<IndexCollModInfo> indexInfo,
                           bool isTimeseries = false) = 0;
    virtual void onDropDatabase(OperationContext* opCtx,
                                const DatabaseName& dbName,
                                bool markFromMigrate) = 0;

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
                                          bool markFromMigrate,
                                          bool isTimeseries = false) = 0;


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
                             const BSONObj& indexInfo,
                             bool isTimeseries = false) = 0;

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
                                             bool stayTemp,
                                             bool markFromMigrate,
                                             bool isTimeseries = false) = 0;

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
                                    bool stayTemp,
                                    bool markFromMigrate,
                                    bool isTimeseries) = 0;

    virtual void onImportCollection(OperationContext* opCtx,
                                    const UUID& importUUID,
                                    const NamespaceString& nss,
                                    long long numRecords,
                                    long long dataSize,
                                    const BSONObj& catalogEntry,
                                    const BSONObj& storageMetadata,
                                    bool isDryRun,
                                    bool isTimeseries) = 0;

    /**
     * The onTransaction Start method is called at the beginning of a multi-document transaction.
     * It must not be called when the transaction is already in progress.
     */
    virtual void onTransactionStart(OperationContext* opCtx) = 0;

    /**
     * The onUnpreparedTransactionCommit method is called on the commit of an unprepared
     * transaction, before the RecoveryUnit onCommit() is called.  It must not be called when no
     * transaction is active.
     *
     * 'reservedSlots' is a list of oplog slots reserved for the oplog entries in a transaction.
     *
     * The 'transactionOperations' contains the list of CRUD operations (formerly 'statements') to
     * be applied in this transaction.
     *
     * The 'applyOpsOperationAssignment' contains a representation of "applyOps" entries and oplog
     * slots to be used for writing pre- and post- image oplog entries for a transaction.
     */
    virtual void onUnpreparedTransactionCommit(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        OpStateAccumulator* opAccumulator = nullptr) = 0;

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
     * are gathered in a single applyOps oplog entry, similar to multi-doc transactions, and written
     * to the oplog.
     */
    virtual void onBatchedWriteCommit(OperationContext* opCtx,
                                      WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                                      OpStateAccumulator* opStateAccumulator = nullptr) = 0;

    /**
     * Clears the accumulated write operations. No further writes is allowed in this storage
     * transaction (WriteUnitOfWork). Calling this function after onBatchedWriteCommit()
     * should be fine for cleanup purposes.
     */
    virtual void onBatchedWriteAbort(OperationContext* opCtx) = 0;

    /**
     * This method is called before an atomic transaction is prepared. It must be called when a
     * transaction is active.
     *
     * 'reservedSlots' is a list of oplog slots reserved for the oplog entries in a transaction. The
     * last reserved slot represents the prepareOpTime used for the prepare oplog entry.
     *
     * The 'transactionOperations' contains the list of CRUD operations to be applied in this
     * transaction. The operations may be modified by setting pre-image and post-image oplog entry
     * timestamps.
     *
     * The 'applyOpsOperationAssignment' contains a representation of "applyOps" entries and oplog
     * slots to be used for writing pre- and post- image oplog entries for a transaction.
     *
     * The 'wallClockTime' is the time to record as wall clock time on oplog entries resulting from
     * transaction preparation.
     */
    virtual void preTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        Date_t wallClockTime) = 0;

    /**
     * The onTransactionPrepare method is called when an atomic transaction is prepared. It must be
     * called when a transaction is active.
     *
     * 'reservedSlots' is a list of oplog slots reserved for the oplog entries in a transaction. The
     * last reserved slot represents the prepareOpTime used for the prepare oplog entry.
     *
     * The 'transactionOperations' contains the list of CRUD operations to be applied in
     * this transaction.
     *
     * The 'applyOpsOperationAssignment' contains a representation of "applyOps" entries and oplog
     * slots to be used for writing pre- and post- image oplog entries for a transaction.
     * The same "applyOps" information should be passed to 'preTransactionPrepare()'.
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
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        OpStateAccumulator* opAccumulator = nullptr) = 0;

    /**
     * The postTransactionPrepare method is called after an atomic transaction is prepared. It must
     * be called when a transaction is active.
     *
     * 'reservedSlots' is a list of oplog slots reserved for the oplog entries in a transaction. The
     * last reserved slot represents the prepareOpTime used for the prepare oplog entry.
     *
     * The 'transactionOperations' contains the list of CRUD operations to be applied in
     * this transaction.
     */
    virtual void postTransactionPrepare(OperationContext* opCtx,
                                        const std::vector<OplogSlot>& reservedSlots,
                                        const TransactionOperations& transactionOperations) = 0;

    /**
     * This method is called when a transaction transitions into prepare while it is not primary,
     * e.g. during secondary oplog application or recoverying prepared transactions from the
     * oplog after restart. The method explicitly requires a session id (i.e. does not use the
     * session id attached to the opCtx) because transaction oplog application currently applies the
     * oplog entries for each prepared transaction in multiple internal sessions acquired from the
     * InternalSessionPool. Currently, those internal sessions are completely unrelated to the
     * session for the transaction itself. For a non-retryable internal transaction, not using the
     * transaction session id in the codepath here can cause the opTime for the transaction to
     * show up in the chunk migration opTime buffer although the writes they correspond to are not
     * retryable and therefore are discarded anyway.
     *
     */
    virtual void onTransactionPrepareNonPrimary(OperationContext* opCtx,
                                                const LogicalSessionId& lsid,
                                                const std::vector<repl::OplogEntry>& statements,
                                                const repl::OpTime& prepareOpTime) = 0;

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

        // Set of all collection UUIDs from ops being rolled back.
        std::set<UUID> rollbackUUIDs = {};

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
     * This method is applicable to the "rollback to a stable timestamp" algorithm.
     *
     * Note: It's not always safe to reload in-memory states in this callback. For in-memory states
     * that choose to reload here instead of ReplicaSetAwareInterface::onConsistentDataAvailable,
     * they'd still be reflecting states in the diverged branch of history during the oplog recovery
     * (from stable timestamp to the common point) phase of rollback. Therefore, we must make sure
     * the oplog recovery phase of rollback doesn't in turn depend on those states being correct.
     * Otherwise, the in-memory states should be reconstructed via
     * ReplicaSetAwareInterface::onConsistentDataAvailable before the oplog recovery phase of
     * rollback so that oplog recovery can work with the reloaded in-memory states that reflect the
     * storage rollback. See replica_set_aware_service.h for more details.
     */
    virtual void onReplicationRollback(OperationContext* opCtx,
                                       const RollbackObserverInfo& rbInfo) = 0;

    /**
     * Called when the majority commit point is updated by replication.
     *
     * This is called while holding a very hot mutex (the ReplicationCoordinator mutex). Therefore
     * it should avoid doing any work that can be done later, and avoid calling back into any
     * replication functions that take this mutex (which would cause self-deadlock).
     */
    virtual void onMajorityCommitPointUpdate(ServiceContext* service,
                                             const repl::OpTime& newCommitPoint) = 0;

    /**
     * Called when the authoritative DSS needs to be updated with a createDatabase operation.
     */
    virtual void onCreateDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) = 0;

    /**
     * Called when the authoritative DSS needs to be updated with a dropDatabase operation.
     */
    virtual void onDropDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) = 0;

    /**
     * Called when 'truncateRange' is called on a collection.
     * Out parameter 'opTime' is updated to the optime of the oplog entry logged.
     */
    virtual void onTruncateRange(OperationContext* opCtx,
                                 const CollectionPtr& coll,
                                 const RecordId& minRecordId,
                                 const RecordId& maxRecordId,
                                 int64_t bytesDeleted,
                                 int64_t docsDeleted,
                                 repl::OpTime& opTime) = 0;

    struct Times;

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

}  // namespace MONGO_MOD_PUB mongo
