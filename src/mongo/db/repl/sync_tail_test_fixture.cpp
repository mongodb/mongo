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

#include "mongo/db/repl/sync_tail_test_fixture.h"

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

void SyncTailOpObserver::onInserts(OperationContext* opCtx,
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

void SyncTailOpObserver::onDelete(OperationContext* opCtx,
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

void SyncTailOpObserver::onCreateCollection(OperationContext* opCtx,
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

// static
OplogApplier::Options SyncTailTest::makeInitialSyncOptions() {
    OplogApplier::Options options(OplogApplication::Mode::kInitialSync);
    options.allowNamespaceNotFoundErrorsOnCrudOps = true;
    return options;
}

OplogApplier::Options SyncTailTest::makeRecoveryOptions() {
    OplogApplier::Options options(OplogApplication::Mode::kRecovering);
    options.allowNamespaceNotFoundErrorsOnCrudOps = true;
    options.skipWritesToOplog = true;
    return options;
}

void SyncTailTest::setUp() {
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

    // Set up an OpObserver to track the documents SyncTail inserts.
    auto opObserver = std::make_unique<SyncTailOpObserver>();
    _opObserver = opObserver.get();
    auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
    opObserverRegistry->addObserver(std::move(opObserver));

    // Initialize the featureCompatibilityVersion server parameter. This is necessary because this
    // test fixture does not create a featureCompatibilityVersion document from which to initialize
    // the server parameter.
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42);
}

void SyncTailTest::tearDown() {
    _opCtx.reset();
    _consistencyMarkers = {};
    DropPendingCollectionReaper::set(serviceContext, {});
    StorageInterface::set(serviceContext, {});
    ServiceContextMongoDTest::tearDown();
}

ReplicationConsistencyMarkers* SyncTailTest::getConsistencyMarkers() const {
    return _consistencyMarkers.get();
}

StorageInterface* SyncTailTest::getStorageInterface() const {
    return StorageInterface::get(serviceContext);
}

void SyncTailTest::_testSyncApplyCrudOperation(ErrorCodes::Error expectedError,
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
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(documentValidationDisabled(_opCtx.get()));
    ASSERT_EQ(
        SyncTail::syncApply(_opCtx.get(), &op, OplogApplication::Mode::kSecondary, boost::none),
        expectedError);
    ASSERT_EQ(applyOpCalled, expectedApplyOpCalled);
}

Status failedApplyCommand(OperationContext* opCtx,
                          const BSONObj& theOperation,
                          OplogApplication::Mode) {
    FAIL("applyCommand unexpectedly invoked.");
    return Status::OK();
}

Status SyncTailTest::runOpSteadyState(const OplogEntry& op) {
    return runOpsSteadyState({op});
}

Status SyncTailTest::runOpsSteadyState(std::vector<OplogEntry> ops) {
    SyncTail syncTail(nullptr,
                      getConsistencyMarkers(),
                      getStorageInterface(),
                      SyncTail::MultiSyncApplyFunc(),
                      nullptr,
                      OplogApplier::Options(OplogApplication::Mode::kSecondary));
    MultiApplier::OperationPtrs opsPtrs;
    for (auto& op : ops) {
        opsPtrs.push_back(&op);
    }
    WorkerMultikeyPathInfo pathInfo;
    return multiSyncApply(_opCtx.get(), &opsPtrs, &syncTail, &pathInfo);
}

Status SyncTailTest::runOpInitialSync(const OplogEntry& op) {
    return runOpsInitialSync({op});
}

Status SyncTailTest::runOpsInitialSync(std::vector<OplogEntry> ops) {
    auto options = makeInitialSyncOptions();
    SyncTail syncTail(nullptr,
                      getConsistencyMarkers(),
                      getStorageInterface(),
                      SyncTail::MultiSyncApplyFunc(),
                      nullptr,
                      options);
    // Apply each operation in a batch of one because 'ops' may contain a mix of commands and CRUD
    // operations provided by idempotency tests.
    for (auto& op : ops) {
        MultiApplier::OperationPtrs opsPtrs;
        opsPtrs.push_back(&op);
        WorkerMultikeyPathInfo pathInfo;
        auto status = multiSyncApply(_opCtx.get(), &opsPtrs, &syncTail, &pathInfo);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status SyncTailTest::runOpPtrsInitialSync(MultiApplier::OperationPtrs ops) {
    auto options = makeInitialSyncOptions();
    SyncTail syncTail(nullptr,
                      getConsistencyMarkers(),
                      getStorageInterface(),
                      SyncTail::MultiSyncApplyFunc(),
                      nullptr,
                      options);
    // Apply each operation in a batch of one because 'ops' may contain a mix of commands and CRUD
    // operations provided by idempotency tests.
    for (auto& op : ops) {
        MultiApplier::OperationPtrs opsPtrs;
        opsPtrs.push_back(op);
        WorkerMultikeyPathInfo pathInfo;
        auto status = multiSyncApply(_opCtx.get(), &opsPtrs, &syncTail, &pathInfo);
        if (!status.isOK()) {
            return status;
        }
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
