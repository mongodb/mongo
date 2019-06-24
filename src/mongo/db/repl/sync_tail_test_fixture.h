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
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/sync_tail.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_txn_record_gen.h"

namespace mongo {

class BSONObj;
class OperationContext;

namespace repl {

/**
 * OpObserver for SyncTail test fixture.
 */
class SyncTailOpObserver : public OpObserverNoop {
public:
    /**
     * This function is called whenever SyncTail inserts documents into a collection.
     */
    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override;

    /**
     * This function is called whenever SyncTail deletes a document from a collection.
     */
    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  bool fromMigrate,
                  const boost::optional<BSONObj>& deletedDoc) override;

    /**
     * Called when SyncTail creates a collection.
     */
    void onCreateCollection(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime) override;

    // Hooks for OpObserver functions. Defaults to a no-op function but may be overridden to check
    // actual documents mutated.
    std::function<void(OperationContext*, const NamespaceString&, const std::vector<BSONObj>&)>
        onInsertsFn;

    std::function<void(OperationContext*,
                       const NamespaceString&,
                       OptionalCollectionUUID,
                       StmtId,
                       bool,
                       const boost::optional<BSONObj>&)>
        onDeleteFn;

    std::function<void(OperationContext*,
                       Collection*,
                       const NamespaceString&,
                       const CollectionOptions&,
                       const BSONObj&)>
        onCreateCollectionFn;
};

class SyncTailTest : public ServiceContextMongoDTest {
public:
    /**
     * Creates OplogApplier::Options for initial sync.
     */
    static OplogApplier::Options makeInitialSyncOptions();
    /**
     * Creates OplogApplier::Options for recovery.
     */
    static OplogApplier::Options makeRecoveryOptions();

protected:
    void _testSyncApplyCrudOperation(ErrorCodes::Error expectedError,
                                     const OplogEntry& op,
                                     bool expectedApplyOpCalled);

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ReplicationConsistencyMarkers> _consistencyMarkers;
    ServiceContext* serviceContext;
    SyncTailOpObserver* _opObserver = nullptr;

    // Implements the SyncTail::MultiSyncApplyFn interface and does nothing.
    static Status noopApplyOperationFn(OperationContext*,
                                       MultiApplier::OperationPtrs*,
                                       SyncTail* st,
                                       WorkerMultikeyPathInfo*) {
        return Status::OK();
    }

    OpTime nextOpTime() {
        static long long lastSecond = 1;
        return OpTime(Timestamp(Seconds(lastSecond++), 0), 1LL);
    }

    void setUp() override;
    void tearDown() override;

    ReplicationConsistencyMarkers* getConsistencyMarkers() const;
    StorageInterface* getStorageInterface() const;

    Status runOpSteadyState(const OplogEntry& op);
    Status runOpsSteadyState(std::vector<OplogEntry> ops);
    Status runOpInitialSync(const OplogEntry& entry);
    Status runOpsInitialSync(std::vector<OplogEntry> ops);
    Status runOpPtrsInitialSync(MultiApplier::OperationPtrs ops);

    UUID kUuid{UUID::gen()};
};

// Utility class to allow easily scanning a collection.  Scans in forward order, returns
// Status::CollectionIsEmpty when scan is exhausted.
class CollectionReader {
public:
    CollectionReader(OperationContext* opCtx, const NamespaceString& nss);

    StatusWith<BSONObj> next();

private:
    AutoGetCollectionForRead _collToScan;
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;
};

Status failedApplyCommand(OperationContext* opCtx,
                          const BSONObj& theOperation,
                          OplogApplication::Mode);

void checkTxnTable(OperationContext* opCtx,
                   const LogicalSessionId& lsid,
                   const TxnNumber& txnNum,
                   const repl::OpTime& expectedOpTime,
                   Date_t expectedWallClock,
                   boost::optional<repl::OpTime> expectedStartOpTime,
                   boost::optional<DurableTxnStateEnum> expectedState);

bool docExists(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc);

}  // namespace repl
}  // namespace mongo
