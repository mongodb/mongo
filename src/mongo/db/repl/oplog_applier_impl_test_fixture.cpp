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

#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/vector_clock_mutable.h"

namespace mongo {
namespace repl {

void OplogApplierImplOpObserver::onInserts(OperationContext* opCtx,
                                           const CollectionPtr& coll,
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
    onInsertsFn(opCtx, coll->ns(), docs);
}

void OplogApplierImplOpObserver::onDelete(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          StmtId stmtId,
                                          const OplogDeleteEntryArgs& args) {
    if (!onDeleteFn) {
        return;
    }
    onDeleteFn(opCtx, nss, uuid, stmtId, args);
}

void OplogApplierImplOpObserver::onUpdate(OperationContext* opCtx,
                                          const OplogUpdateEntryArgs& args) {
    if (!onUpdateFn) {
        return;
    }
    onUpdateFn(opCtx, args);
}

void OplogApplierImplOpObserver::onCreateCollection(OperationContext* opCtx,
                                                    const CollectionPtr& coll,
                                                    const NamespaceString& collectionName,
                                                    const CollectionOptions& options,
                                                    const BSONObj& idIndex,
                                                    const OplogSlot& createOpTime,
                                                    bool fromMigrate) {
    if (!onCreateCollectionFn) {
        return;
    }
    onCreateCollectionFn(opCtx, coll, collectionName, options, idIndex);
}

void OplogApplierImplOpObserver::onRenameCollection(OperationContext* opCtx,
                                                    const NamespaceString& fromCollection,
                                                    const NamespaceString& toCollection,
                                                    const UUID& uuid,
                                                    const boost::optional<UUID>& dropTargetUUID,
                                                    std::uint64_t numRecords,
                                                    bool stayTemp) {
    if (!onRenameCollectionFn) {
        return;
    }
    onRenameCollectionFn(
        opCtx, fromCollection, toCollection, uuid, dropTargetUUID, numRecords, stayTemp);
}

void OplogApplierImplOpObserver::onCreateIndex(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const UUID& uuid,
                                               BSONObj indexDoc,
                                               bool fromMigrate) {
    if (!onCreateIndexFn) {
        return;
    }
    onCreateIndexFn(opCtx, nss, uuid, indexDoc, fromMigrate);
}

void OplogApplierImplOpObserver::onDropIndex(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const UUID& uuid,
                                             const std::string& indexName,
                                             const BSONObj& idxDescriptor) {
    if (!onDropIndexFn) {
        return;
    }
    onDropIndexFn(opCtx, nss, uuid, indexName, idxDescriptor);
}

void OplogApplierImplOpObserver::onCollMod(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const UUID& uuid,
                                           const BSONObj& collModCmd,
                                           const CollectionOptions& oldCollOptions,
                                           boost::optional<IndexCollModInfo> indexInfo) {
    if (!onCollModFn) {
        return;
    }
    onCollModFn(opCtx, nss, uuid, collModCmd, oldCollOptions, indexInfo);
}

void OplogApplierImplTest::setUp() {
    ServiceContextMongoDTest::setUp();

    serviceContext = getServiceContext();
    _opCtx = cc().makeOperationContext();

    ReplicationCoordinator::set(serviceContext,
                                std::make_unique<ReplicationCoordinatorMock>(serviceContext));
    ASSERT_OK(ReplicationCoordinator::get(_opCtx.get())->setFollowerMode(MemberState::RS_PRIMARY));

    StorageInterface::set(serviceContext, std::make_unique<StorageInterfaceImpl>());

    MongoDSessionCatalog::set(
        serviceContext,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    DropPendingCollectionReaper::set(
        serviceContext, std::make_unique<DropPendingCollectionReaper>(getStorageInterface()));
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
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);

    // This is necessary to generate ghost timestamps for index builds that are not 0, since 0 is an
    // invalid timestamp.
    VectorClockMutable::get(_opCtx.get())->tickClusterTimeTo(LogicalTime(Timestamp(1, 0)));
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

// Since applyOplogEntryOrGroupedInserts is being tested outside of its calling function
// (applyOplogBatchPerWorker), we recreate the necessary calling context.
Status OplogApplierImplTest::_applyOplogEntryOrGroupedInsertsWrapper(
    OperationContext* opCtx,
    const OplogEntryOrGroupedInserts& batch,
    OplogApplication::Mode oplogApplicationMode) {
    UnreplicatedWritesBlock uwb(opCtx);
    DisableDocumentValidation validationDisabler(opCtx);
    const bool dataIsConsistent = true;
    return applyOplogEntryOrGroupedInserts(opCtx, batch, oplogApplicationMode, dataIsConsistent);
}

void OplogApplierImplTest::_testApplyOplogEntryOrGroupedInsertsCrudOperation(
    ErrorCodes::Error expectedError,
    const OplogEntry& op,
    const NamespaceString& targetNss,
    bool expectedApplyOpCalled) {
    bool applyOpCalled = false;

    auto checkOpCtx = [&targetNss](OperationContext* opCtx) {
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(targetNss.dbName(), MODE_IX));
        ASSERT_FALSE(opCtx->lockState()->isDbLockedForMode(targetNss.dbName(), MODE_X));
        ASSERT_TRUE(opCtx->lockState()->isCollectionLockedForMode(targetNss, MODE_IX));
        ASSERT_FALSE(opCtx->writesAreReplicated());
        ASSERT_TRUE(DocumentValidationSettings::get(opCtx).isSchemaValidationDisabled());
    };

    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            applyOpCalled = true;
            checkOpCtx(opCtx);
            ASSERT_EQUALS(targetNss, nss);
            ASSERT_EQUALS(1U, docs.size());
            // For upserts we don't know the intended value of the document.
            if (op.getOpType() == repl::OpTypeEnum::kInsert) {
                ASSERT_BSONOBJ_EQ(op.getObject(), docs[0]);
            }
            return Status::OK();
        };

    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const boost::optional<UUID>& uuid,
                                  StmtId stmtId,
                                  const OplogDeleteEntryArgs& args) {
        applyOpCalled = true;
        checkOpCtx(opCtx);
        ASSERT_EQUALS(targetNss, nss);
        ASSERT(args.deletedDoc);
        ASSERT_BSONOBJ_EQ(op.getObject(), *(args.deletedDoc));
        return Status::OK();
    };

    _opObserver->onUpdateFn = [&](OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
        applyOpCalled = true;
        checkOpCtx(opCtx);
        ASSERT_EQUALS(targetNss, args.nss);
        return Status::OK();
    };

    ASSERT_EQ(_applyOplogEntryOrGroupedInsertsWrapper(
                  _opCtx.get(), &op, OplogApplication::Mode::kSecondary),
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
    std::vector<const OplogEntry*> opsPtrs;
    for (auto& op : ops) {
        opsPtrs.push_back(&op);
    }
    WorkerMultikeyPathInfo pathInfo;
    const bool dataIsConsistent = true;
    return oplogApplier.applyOplogBatchPerWorker(
        _opCtx.get(), &opsPtrs, &pathInfo, dataIsConsistent);
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
    // calling applyOplogBatch.
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
        auto applyResult = oplogApplier.applyOplogBatch(_opCtx.get(), {op});
        if (!applyResult.isOK()) {
            std::vector<BSONObj> docsFromOps;
            for (const auto& opForContext : ops) {
                docsFromOps.push_back(opForContext.getEntry().toBSON());
            }
            auto status = applyResult.getStatus();
            return status.withContext(str::stream()
                                      << "failed to apply operation: " << op.toBSONForLogging()
                                      << ". " << BSON("ops" << docsFromOps));
        }
        auto lastApplied = applyResult.getValue();
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
    auto result = client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                                 BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON()));
    ASSERT_FALSE(result.isEmpty());

    auto txnRecord = SessionTxnRecord::parse(IDLParserContext("parse txn record for test"), result);

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
                                            &_collToScan.getCollection(),
                                            PlanYieldPolicy::YieldPolicy::NO_YIELD,
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
    auto result = client.findOne(nss, doc);
    return !result.isEmpty();
}

/**
 * Creates an OplogEntry with given parameters and preset defaults for this test suite.
 */
OplogEntry makeOplogEntry(OpTypeEnum opType,
                          NamespaceString nss,
                          const boost::optional<UUID>& uuid,
                          BSONObj o,
                          boost::optional<BSONObj> o2,
                          boost::optional<bool> fromMigrate) {
    return makeOplogEntry({{1, 1}, 1}, opType, std::move(nss), uuid, o, o2, fromMigrate);
}

OplogEntry makeOplogEntry(OpTime opTime,
                          OpTypeEnum opType,
                          NamespaceString nss,
                          const boost::optional<UUID>& uuid,
                          BSONObj o,
                          boost::optional<BSONObj> o2,
                          boost::optional<bool> fromMigrate) {
    return {DurableOplogEntry(opTime,                     // optime
                              opType,                     // opType
                              std::move(nss),             // namespace
                              uuid,                       // uuid
                              fromMigrate,                // fromMigrate
                              OplogEntry::kOplogVersion,  // version
                              o,                          // o
                              o2,                         // o2
                              {},                         // sessionInfo
                              boost::none,                // upsert
                              Date_t(),                   // wall clock time
                              {},                         // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

OplogEntry makeOplogEntry(OpTypeEnum opType, NamespaceString nss, boost::optional<UUID> uuid) {
    return makeOplogEntry(opType, nss, uuid, BSON("_id" << 0), boost::none);
}

CollectionOptions createOplogCollectionOptions() {
    CollectionOptions options;
    options.capped = true;
    options.cappedSize = 64 * 1024 * 1024LL;
    options.autoIndexId = CollectionOptions::NO;
    return options;
}

CollectionOptions createRecordPreImageCollectionOptions() {
    CollectionOptions options;
    options.recordPreImages = true;
    return options;
}

void createCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const CollectionOptions& options) {
    writeConflictRetry(opCtx, "createCollection", nss.ns(), [&] {
        Lock::DBLock dbLk(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLk(opCtx, nss, MODE_X);

        OldClientContext ctx(opCtx, nss);
        auto db = ctx.db();
        ASSERT_TRUE(db);

        mongo::WriteUnitOfWork wuow(opCtx);
        auto coll = db->createCollection(opCtx, nss, options);
        ASSERT_TRUE(coll);
        wuow.commit();
    });
}

UUID createCollectionWithUuid(OperationContext* opCtx, const NamespaceString& nss) {
    CollectionOptions options;
    options.uuid = UUID::gen();
    createCollection(opCtx, nss, options);
    return options.uuid.value();
}

void createDatabase(OperationContext* opCtx, StringData dbName) {
    Lock::GlobalWrite globalLock(opCtx);
    bool justCreated;
    auto databaseHolder = DatabaseHolder::get(opCtx);
    const DatabaseName tenantDbName(boost::none, dbName);
    auto db = databaseHolder->openDb(opCtx, tenantDbName, &justCreated);
    ASSERT_TRUE(db);
    ASSERT_TRUE(justCreated);
}

bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    return AutoGetCollectionForRead(opCtx, nss).getCollection() != nullptr;
}

void createIndex(OperationContext* opCtx,
                 const NamespaceString& nss,
                 const UUID collUUID,
                 const BSONObj& spec) {
    Lock::DBLock dbLk(opCtx, nss.dbName(), MODE_IX);
    Lock::CollectionLock collLk(opCtx, nss, MODE_X);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    // This fixture sets up some replication, but notably omits installing an OpObserverImpl. This
    // state causes collection creation to timestamp catalog writes, but secondary index creation
    // does not. We use an UnreplicatedWritesBlock to avoid timestamping any of the catalog setup.
    repl::UnreplicatedWritesBlock noRep(opCtx);
    indexBuildsCoord->createIndex(
        opCtx, collUUID, spec, IndexBuildsManager::IndexConstraints::kEnforce, false);
}

}  // namespace repl
}  // namespace mongo
