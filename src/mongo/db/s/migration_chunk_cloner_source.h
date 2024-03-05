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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration_source.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {

class BSONArrayBuilder;
class BSONObjBuilder;
class Collection;
class CollectionPtr;
class Database;
class MigrationChunkClonerSource;
class RecordId;

// Overhead to prevent mods buffers from being too large
const long long kFixedCommandOverhead = 32 * 1024;

/**
 * Used to commit work for LogOpForSharding. Used to keep track of changes in documents that are
 * part of a chunk being migrated.
 */
class LogTransactionOperationsForShardingHandler final : public RecoveryUnit::Change {
public:
    LogTransactionOperationsForShardingHandler(LogicalSessionId lsid,
                                               const std::vector<repl::OplogEntry>& stmts,
                                               repl::OpTime prepareOrCommitOpTime);

    LogTransactionOperationsForShardingHandler(LogicalSessionId lsid,
                                               const std::vector<repl::ReplOperation>& stmts,
                                               repl::OpTime prepareOrCommitOpTime);

    void commit(OperationContext* opCtx, boost::optional<Timestamp>);

    void rollback(OperationContext* opCtx){};

private:
    const LogicalSessionId _lsid;
    std::vector<repl::ReplOperation> _stmts;
    const repl::OpTime _prepareOrCommitOpTime;
};

/**
 * Used to keep track of inserts that can be potentially added as xferMods of a migration.
 */
class LogInsertForShardingHandler final : public RecoveryUnit::Change {
public:
    LogInsertForShardingHandler(NamespaceString nss, BSONObj doc, repl::OpTime opTime);

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) override;

    void rollback(OperationContext* opCtx) override {}

private:
    const NamespaceString _nss;
    const BSONObj _doc;
    const repl::OpTime _opTime;
};

/**
 * Used to keep track of updates that can be potentially added as xferMods of a migration.
 */
class LogUpdateForShardingHandler final : public RecoveryUnit::Change {
public:
    LogUpdateForShardingHandler(NamespaceString nss,
                                boost::optional<BSONObj> preImageDoc,
                                BSONObj postImageDoc,
                                repl::OpTime opTime);

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) override;

    void rollback(OperationContext* opCtx) override {}

private:
    const NamespaceString _nss;
    const boost::optional<BSONObj> _preImageDoc;
    const BSONObj _postImageDoc;
    const repl::OpTime _opTime;
};

/**
 * Used to keep track of deletes that can be potentially added as xferMods of a migration.
 */
class LogDeleteForShardingHandler final : public RecoveryUnit::Change {
public:
    LogDeleteForShardingHandler(NamespaceString nss, DocumentKey documentKey, repl::OpTime opTime);

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) override;

    void rollback(OperationContext* opCtx) override {}

private:
    const NamespaceString _nss;
    const DocumentKey _documentKey;
    const repl::OpTime _opTime;
};

/**
 * Used to keep track of retryable applyOps that may include changes in documents that are
 * part of a chunk being migrated.  Only takes care of retryability; the actual operations
 * are broken out and added to the transfer mods  by the other handlers.
 */
class LogRetryableApplyOpsForShardingHandler final : public RecoveryUnit::Change {
public:
    LogRetryableApplyOpsForShardingHandler(std::vector<NamespaceString> namespaces,
                                           std::vector<repl::OpTime> opTimes);

    void commit(OperationContext* opCtx, boost::optional<Timestamp>);

    void rollback(OperationContext* opCtx){};

private:
    std::vector<NamespaceString> _namespaces;
    std::vector<repl::OpTime> _opTimes;
};

/**
 * This class is responsible for producing chunk documents to be moved from donor to a recipient
 * shard and its methods represent cloning stages. Its lifetime is owned and controlled by a single
 * migration source manager which registers it for notifications from the replication subsystem
 * before calling startClone.
 *
 * Unless explicitly indicated, the methods on this class are not thread-safe.
 *
 * The pattern of using this interface is such that one thread instantiates it and registers it so
 * it begins receiving notifications from the replication subsystem through the
 * on[insert/update/delete]Op methods. It is up to the creator to decide how these methods end up
 * being called, but currently this is done through the CollectionShardingState. The creator then
 * kicks off the cloning as soon as possible by calling startClone.
 */
class MigrationChunkClonerSource {
    MigrationChunkClonerSource(const MigrationChunkClonerSource&) = delete;
    MigrationChunkClonerSource& operator=(const MigrationChunkClonerSource&) = delete;

public:
    MigrationChunkClonerSource(OperationContext* opCtx,
                               const ShardsvrMoveRange& request,
                               const WriteConcernOptions& writeConcern,
                               const BSONObj& shardKeyPattern,
                               ConnectionString donorConnStr,
                               HostAndPort recipientHost);
    ~MigrationChunkClonerSource();

    /**
     * Blocking method, which prepares the object for serving as a source for migrations and tells
     * the recipient shard to start cloning. Before calling this method, this chunk cloner must be
     * registered for notifications from the replication subsystem (not checked here).
     *
     * NOTE: Must be called without any locks and must succeed, before any other methods are called
     * (except for cancelClone and [insert/update/delete]Op).
     */
    Status startClone(OperationContext* opCtx,
                      const UUID& migrationId,
                      const LogicalSessionId& lsid,
                      TxnNumber txnNumber);

    /**
     * Blocking method, which uses some custom selected logic for deciding whether it is appropriate
     * for the donor shard to enter critical section.
     *
     * If it returns a successful status, the caller must as soon as possible stop writes (by
     * entering critical section). On failure it may return any error. Known errors are:
     *  ExceededTimeLimit - if the maxTimeToWait was exceeded
     *
     * NOTE: Must be called without any locks.
     */
    Status awaitUntilCriticalSectionIsAppropriate(OperationContext* opCtx,
                                                  Milliseconds maxTimeToWait);

    /**
     * Tell the recipient shard to commit the documents it has cloned so far. Must be called only
     * when it has been ensured that there will be no more changes happening to documents on the
     * donor shard. If this is not observed, the recipient might miss changes and thus lose data.
     *
     * This must only be called once and no more methods on the cloner must be used afterwards
     * regardless of whether it succeeds or not.
     *
     * Returns statistics about the move. These are informational only and should not be
     * interpreted by the caller for any means other than reporting.
     *
     * NOTE: Must be called without any locks.
     */
    StatusWith<BSONObj> commitClone(OperationContext* opCtx);

    /**
     * Tells the recipient to abort the clone and cleanup any unused data. This method's
     * implementation should be idempotent and never throw.
     *
     * NOTE: Must be called without any locks.
     */
    void cancelClone(OperationContext* opCtx) noexcept;

    /**
     * Notifies this cloner that an insert happened to the collection, which it owns. It is up to
     * the cloner's implementation to decide what to do with this information and it is valid for
     * the implementation to ignore it.
     *
     * NOTE: Must be called with at least IX lock held on the collection.
     */
    void onInsertOp(OperationContext* opCtx,
                    const BSONObj& insertedDoc,
                    const repl::OpTime& opTime);

    /**
     * Notifies this cloner that an update happened to the collection, which it owns. It is up to
     * the cloner's implementation to decide what to do with this information and it is valid for
     * the implementation to ignore it.
     *
     * NOTE: Must be called with at least IX lock held on the collection.
     */
    void onUpdateOp(OperationContext* opCtx,
                    boost::optional<BSONObj> preImageDoc,
                    const BSONObj& postImageDoc,
                    const repl::OpTime& opTime);
    /**
     * Notifies this cloner that a delede happened to the collection, which it owns. It is up to the
     * cloner's implementation to decide what to do with this information and it is valid for the
     * implementation to ignore it.
     *
     * NOTE: Must be called with at least IX lock held on the collection.
     */
    void onDeleteOp(OperationContext* opCtx,
                    const DocumentKey& documentKey,
                    const repl::OpTime& opTime);

    /**
     * Returns the migration session id associated with this cloner, so stale sessions can be
     * disambiguated.
     */
    const MigrationSessionId& getSessionId() const {
        return _sessionId;
    }


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

    /**
     * Returns the number of session oplog entries that were not found but not sent to the
     * destination shard.
     */
    boost::optional<long long> getSessionOplogEntriesSkippedSoFarLowerBound();

    /**
     * Returns the number of session oplog entries that need to be sent to the destination shard.
     */
    boost::optional<long long> getSessionOplogEntriesToBeMigratedSoFar();

private:
    friend class LogOpForShardingHandler;
    friend class LogTransactionOperationsForShardingHandler;
    friend class LogRetryableApplyOpsForShardingHandler;

    using RecordIdSet = std::set<RecordId>;

    /**
     * This is responsible for all the logic revolving around handling documents that needs to be
     * cloned.
     *
     * This class is multithread-safe.
     */
    class CloneList {
    public:
        /**
         * Simple container that increments the given counter when this is constructed and
         * decrements it when it is destroyed. User of this class is responsible for holding
         * necessary mutexes when counter is being modified.
         */
        class InProgressReadToken {
        public:
            InProgressReadToken(WithLock, CloneList& cloneList);
            InProgressReadToken(const InProgressReadToken&) = delete;
            InProgressReadToken(InProgressReadToken&&) = default;

            ~InProgressReadToken();

        private:
            CloneList& _cloneList;
        };

        /**
         * Container for a document that can be added to the nextCloneBatch call. As long as
         * instances of this object exist, it will prevent getNextDoc from prematurely returning
         * an empty response (which means there are no more docs left to clone).
         *
         * This assumes that _mutex is not being held when it is destroyed.
         */
        class DocumentInFlightWhileNotInLock {
        public:
            DocumentInFlightWhileNotInLock(std::unique_ptr<InProgressReadToken> inProgressReadToken,
                                           boost::optional<Snapshotted<BSONObj>> doc);
            DocumentInFlightWhileNotInLock(const DocumentInFlightWhileNotInLock&) = delete;
            DocumentInFlightWhileNotInLock(DocumentInFlightWhileNotInLock&&) = default;

            void setDoc(boost::optional<Snapshotted<BSONObj>> doc);
            const boost::optional<Snapshotted<BSONObj>>& getDoc();

        private:
            std::unique_ptr<InProgressReadToken> _inProgressReadToken;
            boost::optional<Snapshotted<BSONObj>> _doc;
        };

        /**
         * A variant of the DocumentInFlightWhileNotInLock where the _mutex should be held while it
         * has a document contained within it.
         */
        class DocumentInFlightWithLock {
        public:
            DocumentInFlightWithLock(WithLock, CloneList& clonerList);
            DocumentInFlightWithLock(const DocumentInFlightWithLock&) = delete;
            DocumentInFlightWithLock(DocumentInFlightWithLock&&) = default;

            void setDoc(boost::optional<Snapshotted<BSONObj>> doc);

            /**
             * Releases the contained document. Can only be called once for the entire lifetime
             * of this object.
             */
            std::unique_ptr<DocumentInFlightWhileNotInLock> release();

        private:
            std::unique_ptr<InProgressReadToken> _inProgressReadToken;
            boost::optional<Snapshotted<BSONObj>> _doc;
        };

        CloneList();

        /**
         * Overwrites the list of record ids to clone.
         */
        void populateList(RecordIdSet recordIds);

        /**
         * Returns a document to clone. If there are no more documents left to clone,
         * DocumentInFlightWhileNotInLock::getDoc will return boost::none.
         *
         * numRecordsNoLonger exists is an optional parameter that can be used to track
         * the number of recordIds encountered that refers to a document that no longer
         * exists.
         */
        std::unique_ptr<DocumentInFlightWhileNotInLock> getNextDoc(OperationContext* opCtx,
                                                                   const CollectionPtr& collection,
                                                                   int* numRecordsNoLongerExist);

        /**
         * Put back a document previously obtained from this CloneList instance to the overflow
         * pool.
         */
        void insertOverflowDoc(Snapshotted<BSONObj> doc);

        /**
         * Returns true if there are more documents to clone.
         */
        bool hasMore() const;

        /**
         * Returns the size of the populated record ids.
         */
        size_t size() const;

    private:
        /**
         * Increments the counter for inProgressReads.
         */
        void _startedOneInProgressRead(WithLock);

        /**
         * Decrements the counter for inProgressReads.
         */
        void _finishedOneInProgressRead();

        mutable Mutex _mutex = MONGO_MAKE_LATCH("MigrationChunkClonerSource::CloneList::_mutex");

        RecordIdSet _recordIds;

        // This iterator is a pointer into the _recordIds set.  It allows concurrent access to
        // the _recordIds set by allowing threads servicing _migrateClone requests to do the
        // following:
        //   1.  Acquire mutex "_mutex" above.
        //   2.  Copy *_recordIdsIter into its local stack frame.
        //   3.  Increment _recordIdsIter
        //   4.  Unlock "_mutex."
        //   5.  Do the I/O to fetch the document corresponding to this record Id.
        //
        // The purpose of this algorithm, is to allow different threads to concurrently start I/O
        // jobs in order to more fully saturate the disk.
        //
        // One issue with this algorithm, is that only 16MB worth of documents can be returned in
        // response to a _migrateClone request.  But, the thread does not know the size of a
        // document until it does the I/O.  At which point, if the document does not fit in the
        // response to _migrateClone request the document must be made available to a different
        // thread servicing a _migrateClone request. To solve this problem, the thread adds the
        // document to the below _overflowDocs deque.
        RecordIdSet::iterator _recordIdsIter;

        // This deque stores all documents that must be sent to the destination, but could not fit
        // in the response to a particular _migrateClone request.
        std::deque<Snapshotted<BSONObj>> _overflowDocs;

        // This integer represents how many documents are being "held" by threads servicing
        // _migrateClone requests. Any document that is "held" by a thread may be added to the
        // _overflowDocs deque if it doesn't fit in the response to a _migrateClone request.
        // This integer is necessary because it gives us a condition on when all documents to be
        // sent to the destination have been exhausted.
        //
        // If (_recordIdsIter == _recordIds.end() && _overflowDocs.empty() &&
        //     _inProgressReads == 0) then all documents have been returned to the destination.
        int64_t _inProgressReads = 0;

        // This condition variable allows us to wait on the following condition:
        //   Either we're done and the above condition is satisfied, or there is some document to
        //   return.
        stdx::condition_variable _moreDocsCV;
    };

    // Represents the states in which the cloner can be
    enum State { kNew, kCloning, kDone };

    /**
     * Idempotent method, which cleans up any previously initialized state. It is safe to be called
     * at any time, but no methods should be called after it.
     */
    void _cleanup(bool wasSuccessful);

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
     * residing inside the MigrationChunkClonerSource class.
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
     * inside the MigrationChunkClonerSource class.
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

    /**
     * Inspects the pre and post image document keys and determines which xferMods bucket to
     * add a new entry. Returns false if neither pre or post image document keys fall into
     * the chunk boundaries being migrated.
     */
    bool _processUpdateForXferMod(const BSONObj& preImageDocKey, const BSONObj& postImageDocKey);

    /**
     * Defer processing of update ops into xferMods entries to when nextModsBatch is called.
     */
    void _deferProcessingForXferMod(const BSONObj& preImageDocKey);

    /**
     * Converts all deferred update ops captured by the op observer into xferMods entries.
     */
    void _processDeferredXferMods(OperationContext* opCtx);

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
    mutable Mutex _mutex = MONGO_MAKE_LATCH("MigrationChunkClonerSource::_mutex");

    // The current state of the cloner
    State _state{kNew};

    CloneList _cloneList;

    RecordIdSet::size_type _numRecordsCloned{0};
    RecordIdSet::size_type _numRecordsPassedOver{0};

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

    // Amount of ops that are yet to be converted to update/delete xferMods.
    size_t _deferredUntransferredOpsCounter{0};

    // Stores document keys of document that needs to be examined if we need to put in to xferMods
    // list later.
    std::vector<BSONObj> _deferredReloadOrDeletePreImageDocKeys;

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

protected:
    MigrationChunkClonerSource();
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
