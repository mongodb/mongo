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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"

#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"

namespace mongo {
namespace repl {

void OplogApplierImplOpObserver::onInserts(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           OptionalCollectionUUID uuid,
                                           std::vector<InsertStatement>::const_iterator begin,
                                           std::vector<InsertStatement>::const_iterator end,
                                           bool fromMigrate) {
    if (!onInsertsFn) {
        return;
    }
    std::vector<BSONObj> docs;
    for (auto it = begin; it != end; ++it) {
        const InsertStatement& insertStatement = *it;
        docs.push_back(insertStatement.doc.getOwned());
    }
    onInsertsFn(opCtx, nss, docs);
}

void OplogApplierImplOpObserver::onDelete(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          OptionalCollectionUUID uuid,
                                          StmtId stmtId,
                                          bool fromMigrate,
                                          const boost::optional<BSONObj>& deletedDoc) {
    if (!onDeleteFn) {
        return;
    }
    onDeleteFn(opCtx, nss, uuid, stmtId, fromMigrate, deletedDoc);
}

void OplogApplierImplOpObserver::onCreateCollection(OperationContext* opCtx,
                                                    Collection* coll,
                                                    const NamespaceString& collectionName,
                                                    const CollectionOptions& options,
                                                    const BSONObj& idIndex,
                                                    const OplogSlot& createOpTime) {
    if (!onCreateCollectionFn) {
        return;
    }
    onCreateCollectionFn(opCtx, coll, collectionName, options, idIndex);
}
void OplogApplierImplTest::setUp() {
    ServiceContextMongoDTest::setUp();

    serviceContext = getServiceContext();
    _opCtx = cc().makeOperationContext();

    ReplicationCoordinator::set(serviceContext,
                                std::make_unique<ReplicationCoordinatorMock>(serviceContext));
    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));

    StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

    DropPendingCollectionReaper::set(
        serviceContext, std::make_unique<DropPendingCollectionReaper>(getStorageInterface()));
    repl::setOplogCollectionName(serviceContext);
    repl::createOplog(_opCtx.get());

    _consistencyMarkers = std::make_unique<ReplicationConsistencyMarkersMock>();

    // Set up an OpObserver to track the documents OplogApplierImpl inserts.
    auto opObserver = std::make_unique<OplogApplierImplOpObserver>();
    _opObserver = opObserver.get();
    auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
    opObserverRegistry->addObserver(std::move(opObserver));

    // Initialize the featureCompatibilityVersion server parameter. This is necessary because this
    // test fixture does not create a featureCompatibilityVersion document from which to initialize
    // the server parameter.
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44);
}

void OplogApplierImplTest::tearDown() {
    _opCtx.reset();
    _consistencyMarkers = {};
    DropPendingCollectionReaper::set(serviceContext, {});
    StorageInterface::set(serviceContext, {});
    ServiceContextMongoDTest::tearDown();
}

ReplicationConsistencyMarkers* OplogApplierImplTest::getConsistencyMarkers() const {
    return _consistencyMarkers.get();
}

StorageInterface* OplogApplierImplTest::getStorageInterface() const {
    return StorageInterface::get(serviceContext);
}

// Since applyOplogEntryBatch is being tested outside of its calling function (applyOplogGroup), we
// recreate the necessary calling context.
Status OplogApplierImplTest::_applyOplogEntryBatchWrapper(
    OperationContext* opCtx,
    const OplogEntryBatch& batch,
    OplogApplication::Mode oplogApplicationMode) {
    UnreplicatedWritesBlock uwb(opCtx);
    DisableDocumentValidation validationDisabler(opCtx);
    return applyOplogEntryBatch(opCtx, batch, oplogApplicationMode);
}

void OplogApplierImplTest::_testApplyOplogEntryBatchCrudOperation(ErrorCodes::Error expectedError,
                                                                  const OplogEntry& op,
                                                                  bool expectedApplyOpCalled) {
    bool applyOpCalled = false;

    auto checkOpCtx = [](OperationContext* opCtx) {
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode("test", MODE_IX));
        ASSERT_FALSE(opCtx->lockState()->isDbLockedForMode("test", MODE_X));
        ASSERT_TRUE(
            opCtx->lockState()->isCollectionLockedForMode(NamespaceString("test.t"), MODE_IX));
        ASSERT_FALSE(opCtx->writesAreReplicated());
        ASSERT_TRUE(documentValidationDisabled(opCtx));
    };

    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            applyOpCalled = true;
            checkOpCtx(opCtx);
            ASSERT_EQUALS(NamespaceString("test.t"), nss);
            ASSERT_EQUALS(1U, docs.size());
            ASSERT_BSONOBJ_EQ(op.getObject(), docs[0]);
            return Status::OK();
        };

    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  OptionalCollectionUUID uuid,
                                  StmtId stmtId,
                                  bool fromMigrate,
                                  const boost::optional<BSONObj>& deletedDoc) {
        applyOpCalled = true;
        checkOpCtx(opCtx);
        ASSERT_EQUALS(NamespaceString("test.t"), nss);
        ASSERT(deletedDoc);
        ASSERT_BSONOBJ_EQ(op.getObject(), *deletedDoc);
        return Status::OK();
    };

    ASSERT_EQ(_applyOplogEntryBatchWrapper(_opCtx.get(), &op, OplogApplication::Mode::kSecondary),
              expectedError);
    ASSERT_EQ(applyOpCalled, expectedApplyOpCalled);
}

Status failedApplyCommand(OperationContext* opCtx,
                          const BSONObj& theOperation,
                          OplogApplication::Mode) {
    FAIL("applyCommand unexpectedly invoked.");
    return Status::OK();
}

Status OplogApplierImplTest::runOpSteadyState(const OplogEntry& op) {
    return runOpsSteadyState({op});
}

Status OplogApplierImplTest::runOpsSteadyState(std::vector<OplogEntry> ops) {
    TestApplyOplogGroupApplier oplogApplier(
        getConsistencyMarkers(),
        getStorageInterface(),
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary));
    MultiApplier::OperationPtrs opsPtrs;
    for (auto& op : ops) {
        opsPtrs.push_back(&op);
    }
    WorkerMultikeyPathInfo pathInfo;
    return oplogApplier.applyOplogGroup(_opCtx.get(), &opsPtrs, &pathInfo);
}

Status OplogApplierImplTest::runOpInitialSync(const OplogEntry& op) {
    return runOpsInitialSync({op});
}

Status OplogApplierImplTest::runOpsInitialSync(std::vector<OplogEntry> ops) {
    NoopOplogApplierObserver observer;
    auto storageInterface = getStorageInterface();
    auto writerPool = makeReplWriterPool();
    OplogApplierImpl oplogApplier(
        nullptr,  // executor
        nullptr,  // oplogBuffer
        &observer,
        ReplicationCoordinator::get(_opCtx.get()),
        getConsistencyMarkers(),
        storageInterface,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kInitialSync),
        writerPool.get());
    // Idempotency tests apply the same batch of oplog entries multiple times in a loop, which would
    // result in out-of-order oplog inserts. So we truncate the oplog collection first before
    // calling multiApply.
    ASSERT_OK(
        storageInterface->truncateCollection(_opCtx.get(), NamespaceString::kRsOplogNamespace));
    // Apply each operation in a batch of one because 'ops' may contain a mix of commands and CRUD
    // operations provided by idempotency tests. Applying operations in a batch of one is also
    // necessary to work around oplog visibility issues. For example, idempotency tests may contain
    // a prepare and a commit that we don't apply both in the same batch in production oplog
    // application because the commit needs to read the prepare entry. So we apply each operation in
    // its own batch and update oplog visibility after each batch to make sure all previously
    // applied entries are visible to subsequent batches.
    for (auto& op : ops) {
        auto status = oplogApplier.multiApply(_opCtx.get(), {op});
        if (!status.isOK()) {
            return status.getStatus();
        }
        auto lastApplied = status.getValue();
        const bool orderedCommit = true;
        // Update oplog visibility by notifying the storage engine of the new oplog entries.
        storageInterface->oplogDiskLocRegister(
            _opCtx.get(), lastApplied.getTimestamp(), orderedCommit);
    }
    return Status::OK();
}

void checkTxnTable(OperationContext* opCtx,
                   const LogicalSessionId& lsid,
                   const TxnNumber& txnNum,
                   const repl::OpTime& expectedOpTime,
                   Date_t expectedWallClock,
                   boost::optional<repl::OpTime> expectedStartOpTime,
                   boost::optional<DurableTxnStateEnum> expectedState) {
    DBDirectClient client(opCtx);
    auto result = client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                                 {BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON())});
    ASSERT_FALSE(result.isEmpty());

    auto txnRecord =
        SessionTxnRecord::parse(IDLParserErrorContext("parse txn record for test"), result);

    ASSERT_EQ(txnNum, txnRecord.getTxnNum());
    ASSERT_EQ(expectedOpTime, txnRecord.getLastWriteOpTime());
    ASSERT_EQ(expectedWallClock, txnRecord.getLastWriteDate());
    if (expectedStartOpTime) {
        ASSERT(txnRecord.getStartOpTime());
        ASSERT_EQ(*expectedStartOpTime, *txnRecord.getStartOpTime());
    } else {
        ASSERT(!txnRecord.getStartOpTime());
    }
    if (expectedState) {
        ASSERT(*expectedState == txnRecord.getState());
    }
}

CollectionReader::CollectionReader(OperationContext* opCtx, const NamespaceString& nss)
    : _collToScan(opCtx, nss),
      _exec(InternalPlanner::collectionScan(opCtx,
                                            nss.ns(),
                                            _collToScan.getCollection(),
                                            PlanExecutor::NO_YIELD,
                                            InternalPlanner::FORWARD)) {}

StatusWith<BSONObj> CollectionReader::next() {
    BSONObj obj;

    auto state = _exec->getNext(&obj, nullptr);
    if (state == PlanExecutor::IS_EOF) {
        return {ErrorCodes::CollectionIsEmpty,
                str::stream() << "no more documents in " << _collToScan.getNss()};
    }

    // PlanExecutors that do not yield should only return ADVANCED or EOF.
    invariant(state == PlanExecutor::ADVANCED);
    return obj;
}

bool docExists(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
    DBDirectClient client(opCtx);
    auto result = client.findOne(nss.ns(), {doc});
    return !result.isEmpty();
}
}  // namespace repl
}  // namespace mongo
