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
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_txn_record_gen.h"

namespace mongo {

class BSONObj;
class OperationContext;

namespace repl {

/**
 * Test only subclass of OplogApplierImpl that makes applyOplogGroup a public method.
 */
class TestApplyOplogGroupApplier : public OplogApplierImpl {
public:
    TestApplyOplogGroupApplier(ReplicationConsistencyMarkers* consistencyMarkers,
                               StorageInterface* storageInterface,
                               const OplogApplier::Options& options)
        : OplogApplierImpl(nullptr,
                           nullptr,
                           nullptr,
                           nullptr,
                           consistencyMarkers,
                           storageInterface,
                           options,
                           nullptr) {}
    using OplogApplierImpl::applyOplogGroup;
};

/**
 * OpObserver for OplogApplierImpl test fixture.
 */
class OplogApplierImplOpObserver : public OpObserverNoop {
public:
    /**
     * This function is called whenever OplogApplierImpl inserts documents into a collection.
     */
    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   OptionalCollectionUUID uuid,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   bool fromMigrate) override;

    /**
     * This function is called whenever OplogApplierImpl deletes a document from a collection.
     */
    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  OptionalCollectionUUID uuid,
                  StmtId stmtId,
                  bool fromMigrate,
                  const boost::optional<BSONObj>& deletedDoc) override;

    /**
     * Called when OplogApplierImpl creates a collection.
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

class OplogApplierImplTest : public ServiceContextMongoDTest {
protected:
    void _testApplyOplogEntryBatchCrudOperation(ErrorCodes::Error expectedError,
                                                const OplogEntry& op,
                                                bool expectedApplyOpCalled);

    Status _applyOplogEntryBatchWrapper(OperationContext* opCtx,
                                        const OplogEntryBatch& batch,
                                        OplogApplication::Mode oplogApplicationMode);

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ReplicationConsistencyMarkers> _consistencyMarkers;
    ServiceContext* serviceContext;
    OplogApplierImplOpObserver* _opObserver = nullptr;

    OpTime nextOpTime() {
        static long long lastSecond = 1;
        return OpTime(Timestamp(Seconds(lastSecond++), 0), 1LL);
    }

    void setUp() override;
    void tearDown() override;


    ReplicationCoordinator* getReplCoord() const;
    ReplicationConsistencyMarkers* getConsistencyMarkers() const;
    StorageInterface* getStorageInterface() const;

    Status runOpSteadyState(const OplogEntry& op);
    Status runOpsSteadyState(std::vector<OplogEntry> ops);
    Status runOpInitialSync(const OplogEntry& entry);
    Status runOpsInitialSync(std::vector<OplogEntry> ops);

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
