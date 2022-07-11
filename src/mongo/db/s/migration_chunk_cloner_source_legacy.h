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

#include <list>
#include <memory>
#include <set>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class BSONArrayBuilder;
class BSONObjBuilder;
class Collection;
class CollectionPtr;
class Database;
class RecordId;

// Overhead to prevent mods buffers from being too large
const long long kFixedCommandOverhead = 32 * 1024;

/**
 * Used to commit work for LogOpForSharding. Used to keep track of changes in documents that are
 * part of a chunk being migrated.
 */
class LogTransactionOperationsForShardingHandler final : public RecoveryUnit::Change {
public:
    /**
     * Invariant: idObj should belong to a document that is part of the active chunk being migrated
     */
    LogTransactionOperationsForShardingHandler(const LogicalSessionId lsid,
                                               const std::vector<repl::ReplOperation>& stmts,
                                               const repl::OpTime& prepareOrCommitOpTime)
        : _lsid(lsid), _stmts(stmts), _prepareOrCommitOpTime(prepareOrCommitOpTime) {}

    void commit(boost::optional<Timestamp>) override;

    void rollback() override{};

private:
    const LogicalSessionId _lsid;
    std::vector<repl::ReplOperation> _stmts;
    const repl::OpTime _prepareOrCommitOpTime;
};

class MigrationChunkClonerSourceLegacy final : public MigrationChunkClonerSource {
    MigrationChunkClonerSourceLegacy(const MigrationChunkClonerSourceLegacy&) = delete;
    MigrationChunkClonerSourceLegacy& operator=(const MigrationChunkClonerSourceLegacy&) = delete;

public:
    MigrationChunkClonerSourceLegacy(const ShardsvrMoveRange& request,
                                     const WriteConcernOptions& writeConcern,
                                     const BSONObj& shardKeyPattern,
                                     ConnectionString donorConnStr,
                                     HostAndPort recipientHost);
    ~MigrationChunkClonerSourceLegacy();

    Status startClone(OperationContext* opCtx,
                      const UUID& migrationId,
                      const LogicalSessionId& lsid,
                      TxnNumber txnNumber) override;

    Status awaitUntilCriticalSectionIsAppropriate(OperationContext* opCtx,
                                                  Milliseconds maxTimeToWait) override;

    StatusWith<BSONObj> commitClone(OperationContext* opCtx) override;

    void cancelClone(OperationContext* opCtx) noexcept override;

    bool isDocumentInMigratingChunk(const BSONObj& doc) override;

    void onInsertOp(OperationContext* opCtx,
                    const BSONObj& insertedDoc,
                    const repl::OpTime& opTime) override;

    void onUpdateOp(OperationContext* opCtx,
                    boost::optional<BSONObj> preImageDoc,
                    const BSONObj& postImageDoc,
                    const repl::OpTime& opTime,
                    const repl::OpTime& prePostImageOpTime) override;

    void onDeleteOp(OperationContext* opCtx,
                    const BSONObj& deletedDocId,
                    const repl::OpTime& opTime,
                    const repl::OpTime& preImageOpTime) override;

    const MigrationSessionId& getSessionId() const override {
        return _sessionId;
    }

    // Legacy cloner specific functionality

    /**
     * Returns the rollback ID recorded at the beginning of session migration. If the underlying
     * SessionCatalogMigrationSource does not exist, that means this node is running as a standalone
     * and doesn't support retryable writes, so we return boost::none.
     */
    boost::optional<int> getRollbackIdAtInit() const {
        if (_sessionCatalogSource) {
            return _sessionCatalogSource->getRollbackIdAtInit();
        }
        return boost::none;
    }

    /**
     * Called by the recipient shard. Used to estimate how many more bytes of clone data are
     * remaining in the chunk cloner.
     */
    uint64_t getCloneBatchBufferAllocationSize();

    /**
     * Called by the recipient shard. Populates the passed BSONArrayBuilder with a set of documents,
     * which are part of the initial clone sequence. Assumes that there is only one active caller
     * to this method at a time (otherwise, it can cause corruption/crash).
     *
     * Returns OK status on success. If there were documents returned in the result argument, this
     * method should be called more times until the result is empty. If it returns failure, it is
     * not safe to call more methods on this class other than cancelClone.
     *
     * This method will return early if too much time is spent fetching the documents in order to
     * give a chance to the caller to perform some form of yielding. It does not free or acquire any
     * locks on its own.
     *
     * NOTE: Must be called with the collection lock held in at least IS mode.
     */
    Status nextCloneBatch(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          BSONArrayBuilder* arrBuilder);

    /**
     * Called by the recipient shard. Transfers the accummulated local mods from source to
     * destination. Must not be called before all cloned objects have been fetched through calls to
     * nextCloneBatch.
     *
     * NOTE: Must be called with the collection lock held in at least IS mode.
     */
    Status nextModsBatch(OperationContext* opCtx, BSONObjBuilder* builder);

    /**
     * Appends to 'arrBuilder' oplog entries which wrote to the currently migrated chunk and contain
     * session information.
     *
     * If this function returns a valid OpTime, this means that the oplog appended are not
     * guaranteed to be majority committed and the caller has to wait for the returned opTime to be
     * majority committed before returning them to the donor shard.
     *
     * If the underlying SessionCatalogMigrationSource does not exist, that means this node is
     * running as a standalone and doesn't support retryable writes, so we return boost::none.
     *
     * This waiting is necessary because session migration is only allowed to send out committed
     * entries, as opposed to chunk migration, which can send out uncommitted documents. With chunk
     * migration, the uncommitted documents will not be visibile until the end of the migration
     * commits, which means that if it fails, they won't be visible, whereas session oplog entries
     * take effect immediately since they are appended to the chain.
     */
    boost::optional<repl::OpTime> nextSessionMigrationBatch(OperationContext* opCtx,
                                                            BSONArrayBuilder* arrBuilder);

    /**
     * Returns a notification that can be used to wait for new oplog that needs to be migrated.
     * If the value in the notification returns true, it means that there are no more new batches
     * that needs to be fetched because the migration has already entered the critical section or
     * aborted.
     *
     * Returns nullptr if there is no session migration associated with this migration.
     */
    std::shared_ptr<Notification<bool>> getNotificationForNextSessionMigrationBatch();

    const NamespaceString& nss() {
        return _args.getCommandParameter();
    }

    const BSONObj& getMin() {
        invariant(_args.getMin());
        return *_args.getMin();
    }

    const BSONObj& getMax() {
        invariant(_args.getMax());
        return *_args.getMax();
    }

private:
    friend class LogOpForShardingHandler;
    friend class LogTransactionOperationsForShardingHandler;

    // Represents the states in which the cloner can be
    enum State { kNew, kCloning, kDone };

    /**
     * Idempotent method, which cleans up any previously initialized state. It is safe to be called
     * at any time, but no methods should be called after it.
     */
    void _cleanup();

    /**
     * Synchronously invokes the recipient shard with the specified command and either returns the
     * command response (if succeeded) or the status, if the command failed.
     */
    StatusWith<BSONObj> _callRecipient(OperationContext* opCtx, const BSONObj& cmdObj);

    StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> _getIndexScanExecutor(
        OperationContext* opCtx,
        const CollectionPtr& collection,
        InternalPlanner::IndexScanOptions scanOption);

    void _nextCloneBatchFromIndexScan(OperationContext* opCtx,
                                      const CollectionPtr& collection,
                                      BSONArrayBuilder* arrBuilder);

    void _nextCloneBatchFromCloneRecordIds(OperationContext* opCtx,
                                           const CollectionPtr& collection,
                                           BSONArrayBuilder* arrBuilder);

    /**
     * Get the recordIds that belong to the chunk migrated and sort them in _cloneRecordIds (to
     * avoid seeking disk later).
     *
     * Returns OK or any error status otherwise.
     */
    Status _storeCurrentRecordId(OperationContext* opCtx);

    /**
     * Adds the OpTime to the list of OpTimes for oplog entries that we should consider migrating as
     * part of session migration.
     */
    void _addToSessionMigrationOptimeQueue(
        const repl::OpTime& opTime,
        SessionCatalogMigrationSource::EntryAtOpTimeType entryAtOpTimeType);

    void _addToSessionMigrationOptimeQueueForTransactionCommit(
        const repl::OpTime& opTime,
        SessionCatalogMigrationSource::EntryAtOpTimeType entryAtOpTimeType);

    /*
     * Appends the relevant document changes to the appropriate internal data structures (known
     * colloquially as the 'transfer mods queue'). These structures track document changes that are
     * part of a part of a chunk being migrated. In doing so, this the method also removes the
     * corresponding operation track request from the operation track requests queue.
     */
    void _addToTransferModsQueue(const BSONObj& idObj, char op, const repl::OpTime& opTime);

    /**
     * Adds an operation to the outstanding operation track requests. Returns false if the cloner
     * is no longer accepting new operation track requests.
     */
    bool _addedOperationToOutstandingOperationTrackRequests();

    /**
     * Called to indicate a request to track an operation must be filled. The operations in
     * question indicate a change to a document in the chunk being cloned. Increments a counter
     * residing inside the MigrationChunkClonerSourceLegacy class.
     *
     * There should always be a one to one match from the number of calls to this function to the
     * number of calls to the corresponding decrement* function.
     *
     * NOTE: This funtion invariants that we are currently accepting new operation track requests.
     * It is up to callers of this function to make sure that will always be the case.
     */
    void _incrementOutstandingOperationTrackRequests(WithLock);

    /**
     * Called once a request to track an operation has been filled. The operations in question
     * indicate a change to a document in the chunk being cloned. Decrements a counter residing
     * inside the MigrationChunkClonerSourceLegacy class.
     *
     * There should always be a one to one match from the number of calls to this function to the
     * number of calls to the corresponding increment* function.
     */
    void _decrementOutstandingOperationTrackRequests();

    /**
     * Waits for all outstanding operation track requests to be fulfilled before returning from this
     * function. Should only be used in the cleanup for this class. Should use a lock wrapped
     * around this class's mutex.
     */
    void _drainAllOutstandingOperationTrackRequests(stdx::unique_lock<Latch>& lk);

    /**
     * Sends _recvChunkStatus to the recipient shard until it receives 'steady' from the recipient,
     * an error has occurred, or a timeout is hit.
     */
    Status _checkRecipientCloningStatus(OperationContext* opCtx, Milliseconds maxTimeToWait);

    // The original move range request
    const ShardsvrMoveRange _args;

    // The write concern associated with the move range
    const WriteConcernOptions _writeConcern;

    // The shard key associated with the namespace
    const ShardKeyPattern _shardKeyPattern;

    // The migration session id
    const MigrationSessionId _sessionId;

    // The resolved connection string of the donor shard
    const ConnectionString _donorConnStr;

    // The resolved primary of the recipient shard
    const HostAndPort _recipientHost;

    std::unique_ptr<SessionCatalogMigrationSource> _sessionCatalogSource;

    // Protects the entries below
    Mutex _mutex = MONGO_MAKE_LATCH("MigrationChunkClonerSourceLegacy::_mutex");

    // The current state of the cloner
    State _state{kNew};

    // List of record ids that needs to be transferred (initial clone)
    std::set<RecordId> _cloneRecordIds;

    // The estimated average object size during the clone phase. Used for buffer size
    // pre-allocation (initial clone).
    uint64_t _averageObjectSizeForCloneRecordIds{0};

    // The estimated average object _id size during the clone phase.
    uint64_t _averageObjectIdSize{0};

    // Represents all of the requested but not yet fulfilled operations to be tracked, with regards
    // to the chunk being cloned.
    uint64_t _outstandingOperationTrackRequests{0};

    // Signals to any waiters once all unresolved operation tracking requests have completed.
    stdx::condition_variable _allOutstandingOperationTrackRequestsDrained;

    // Indicates whether new requests to track an operation are accepted.
    bool _acceptingNewOperationTrackRequests{true};

    // List of _id of documents that were modified that must be re-cloned (xfer mods)
    std::list<BSONObj> _reload;

    // Amount of upsert xfer mods that have not yet reached the recipient.
    size_t _untransferredUpsertsCounter{0};

    // List of _id of documents that were deleted during clone that should be deleted later (xfer
    // mods)
    std::list<BSONObj> _deleted;

    // Amount of delete xfer mods that have not yet reached the recipient.
    size_t _untransferredDeletesCounter{0};

    // Total bytes in _reload + _deleted (xfer mods)
    uint64_t _memoryUsed{0};

    // False if the move chunk request specified ForceJumbo::kDoNotForce, true otherwise.
    const bool _forceJumbo;

    struct JumboChunkCloneState {
        // Plan executor for collection scan used to clone docs.
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> clonerExec;

        // The current state of 'clonerExec'.
        PlanExecutor::ExecState clonerState;

        // Number docs in jumbo chunk cloned so far
        int docsCloned = 0;
    };

    // Set only once its discovered a chunk is jumbo
    boost::optional<JumboChunkCloneState> _jumboChunkCloneState;
};

/**
 * Appends to the builder the list of documents either deleted or modified during migration.
 * Entries appended to the builder are removed from the list.
 * Returns the total size of the documents that were appended + initialSize.
 */
long long xferMods(BSONArrayBuilder* arr,
                   std::list<BSONObj>* modsList,
                   long long initialSize,
                   std::function<bool(BSONObj, BSONObj*)> extractDocToAppendFn);

}  // namespace mongo
