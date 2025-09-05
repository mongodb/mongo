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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/index/multikey_metadata_access_stats.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_builds/index_build_entry_helpers.h"
#include "mongo/db/index_builds/index_build_interceptor.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/index_builds/skipped_record_tracker.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/drop_database.h"
#include "mongo/db/local_catalog/drop_indexes.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/durable_catalog_entry_metadata.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

Status createIndexFromSpec(OperationContext* opCtx,
                           VectorClockMutable* clock,
                           StringData ns,
                           const BSONObj& spec) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);

    // Make sure we haven't already locked this namespace. An AutoGetCollection already instantiated
    // on this namespace would have a dangling Collection pointer after this function has run.
    invariant(!shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));

    AutoGetDb autoDb(opCtx, nss.dbName(), MODE_X);
    {
        WriteUnitOfWork wunit(opCtx);
        CollectionWriter writer{opCtx, nss};
        auto coll = writer.getWritableCollection(opCtx);
        if (!coll) {
            auto db = autoDb.ensureDbExists(opCtx);
            invariant(db);
            coll = db->createCollection(opCtx, NamespaceString::createNamespaceString_forTest(ns));
        }
        invariant(coll);
        wunit.commit();
    }

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto indexBuildInfo =
        IndexBuildInfo(spec, *storageEngine, nss.dbName(), VersionContext::getDecoration(opCtx));
    MultiIndexBlock indexer;
    CollectionWriter collection(opCtx, nss);
    ScopeGuard abortOnExit(
        [&] { indexer.abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn); });
    Status status =
        indexer
            .init(
                opCtx,
                collection,
                {indexBuildInfo},
                [opCtx, clock] {
                    if (opCtx->writesAreReplicated() &&
                        shard_role_details::getRecoveryUnit(opCtx)->getCommitTimestamp().isNull()) {
                        uassertStatusOK(shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(
                            clock->tickClusterTime(1).asTimestamp()));
                    }
                })
            .getStatus();
    if (status == ErrorCodes::IndexAlreadyExists) {
        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }
    status = indexer.insertAllDocumentsInCollection(opCtx, nss);
    if (!status.isOK()) {
        return status;
    }
    status = indexer.retrySkippedRecords(opCtx, collection.get());
    if (!status.isOK()) {
        return status;
    }
    status = indexer.checkConstraints(opCtx, collection.get());
    if (!status.isOK()) {
        return status;
    }
    WriteUnitOfWork wunit(opCtx);
    ASSERT_OK(indexer.commit(opCtx,
                             collection.getWritableCollection(opCtx),
                             MultiIndexBlock::kNoopOnCreateEachFn,
                             MultiIndexBlock::kNoopOnCommitFn));
    if (opCtx->writesAreReplicated()) {
        LogicalTime indexTs = clock->tickClusterTime(1);
        ASSERT_OK(shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(indexTs.asTimestamp()));
    }
    wunit.commit();
    abortOnExit.dismiss();
    return Status::OK();
}

CollectionAcquisition acquireCollForRead(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kRead),
        MODE_IS);
}

/**
 * RAII type for operating at a timestamp. Will remove any timestamping when the object destructs.
 */
class OneOffRead {
public:
    OneOffRead(OperationContext* opCtx, const Timestamp& ts, bool waitForOplog = false)
        : _opCtx(opCtx) {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        if (waitForOplog) {
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            LocalOplogInfo* oplogInfo = LocalOplogInfo::get(opCtx);

            // Oplog should be available in this test.
            invariant(oplogInfo);
            storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx,
                                                                   oplogInfo->getRecordStore());
        }
        if (ts.isNull()) {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
        } else {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kProvided, ts);
        }
    }

    ~OneOffRead() {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kNoTimestamp);
    }

private:
    OperationContext* _opCtx;
};

class IgnorePrepareBlock {
public:
    IgnorePrepareBlock(OperationContext* opCtx) : _opCtx(opCtx) {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(_opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflicts);
    }

    ~IgnorePrepareBlock() {
        shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        shard_role_details::getRecoveryUnit(_opCtx)->setPrepareConflictBehavior(
            PrepareConflictBehavior::kEnforce);
    }

private:
    OperationContext* _opCtx;
};
}  // namespace

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

void assertIndexMetaDataMissing(std::shared_ptr<durable_catalog::CatalogEntryMetaData> collMetaData,
                                StringData indexName) {
    const auto idxOffset = collMetaData->findIndexOffset(indexName);
    ASSERT_EQUALS(-1, idxOffset) << indexName << ". Collection Metdata: " << collMetaData->toBSON();
}

durable_catalog::CatalogEntryMetaData::IndexMetaData getIndexMetaData(
    std::shared_ptr<durable_catalog::CatalogEntryMetaData> collMetaData, StringData indexName) {
    const auto idxOffset = collMetaData->findIndexOffset(indexName);
    ASSERT_GT(idxOffset, -1) << indexName;
    return collMetaData->indexes[idxOffset];
}

class DoNothingOplogApplierObserver : public repl::OplogApplier::Observer {
public:
    void onBatchBegin(const std::vector<repl::OplogEntry>&) final {}
    void onBatchEnd(const StatusWith<repl::OpTime>&, const std::vector<repl::OplogEntry>&) final {}
};

class StorageTimestampTest : public ServiceContextMongoDTest {
public:
    ServiceContext::UniqueOperationContext _opCtxRaii;
    OperationContext* _opCtx;
    VectorClockMutable* _clock;

    // Set up Timestamps in the past, present, and future.
    LogicalTime _pastLt;
    Timestamp _pastTs;
    LogicalTime _presentLt;
    Timestamp _presentTs;
    LogicalTime _futureLt;
    Timestamp _futureTs;
    Timestamp _nullTs;
    int _presentTerm;
    repl::ReplicationCoordinatorMock* _coordinatorMock;
    repl::ReplicationConsistencyMarkers* _consistencyMarkers;

    StorageTimestampTest()
        : ServiceContextMongoDTest(Options{}.useReplSettings(true).useMockClock(true)) {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        _opCtxRaii = cc().makeOperationContext();
        _opCtx = _opCtxRaii.get();
        _clock = VectorClockMutable::get(_opCtx);

        // Set up Timestamps in the past, present, and future.
        _pastLt = _clock->tickClusterTime(1);
        _pastTs = _pastLt.asTimestamp();
        _presentLt = _clock->tickClusterTime(1);
        _presentTs = _presentLt.asTimestamp();
        // Without the const, the wrong addTicks overload is used.
        _futureLt = const_cast<const LogicalTime&>(_presentLt).addTicks(1);
        _futureTs = _futureLt.asTimestamp();
        _nullTs = Timestamp();
        _presentTerm = 1;

        auto coordinatorMock = new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(),
                                                                    getGlobalReplSettings());
        _coordinatorMock = coordinatorMock;
        coordinatorMock->alwaysAllowWrites(true);
        repl::ReplicationCoordinator::set(
            _opCtx->getServiceContext(),
            std::unique_ptr<repl::ReplicationCoordinator>(coordinatorMock));
        repl::StorageInterface::set(_opCtx->getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());

        auto replicationProcess = new repl::ReplicationProcess(
            repl::StorageInterface::get(_opCtx->getServiceContext()),
            std::make_unique<repl::ReplicationConsistencyMarkersMock>(),
            std::make_unique<repl::ReplicationRecoveryMock>());
        repl::ReplicationProcess::set(
            cc().getServiceContext(),
            std::unique_ptr<repl::ReplicationProcess>(replicationProcess));

        MongoDSessionCatalog::set(
            _opCtx->getServiceContext(),
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        _consistencyMarkers =
            repl::ReplicationProcess::get(cc().getServiceContext())->getConsistencyMarkers();

        // Since the Client object persists across tests, even though the global
        // ReplicationCoordinator does not, we need to clear the last op associated with the client
        // to avoid the invariant in ReplClientInfo::setLastOp that the optime only goes forward.
        repl::ReplClientInfo::forClient(_opCtx->getClient()).clearLastOp();

        opObserverRegistry()->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        repl::createOplog(_opCtx);

        _clock->tickClusterTimeTo(LogicalTime(Timestamp(1, 0)));

        ASSERT_EQUALS(_presentTs,
                      const_cast<const LogicalTime&>(_pastLt).addTicks(1).asTimestamp());
        setReplCoordAppliedOpTime(repl::OpTime(_presentTs, _presentTerm));

        {
            // These tests were developed prior to needing an index build entry collection. Use an
            // unreplicated write block to avoid changing oplog state due to collection creation.
            repl::UnreplicatedWritesBlock unreplicated(_opCtx);
            WriteUnitOfWork wuow(_opCtx);
            mongo::indexbuildentryhelpers::ensureIndexBuildEntriesNamespaceExists(_opCtx);
            wuow.commit();
        }
    }

    void create(NamespaceString nss) const {
        ::mongo::writeConflictRetry(_opCtx, "deleteAll", nss, [&] {
            shard_role_details::getRecoveryUnit(_opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
            shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
            AutoGetDb autoDb(_opCtx, nss.dbName(), MODE_IX);
            auto coll1 = acquireCollection(_opCtx,
                                           CollectionAcquisitionRequest::fromOpCtx(
                                               _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                           MODE_X);
            // Only creating new namespaces is supported. We restart the storage engine across all
            // tests. If a test wants to remove data from existing collections, care must be taken
            // to timestamp those deletes appropriately.
            invariant(!coll1.exists());
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_IX);
            auto db = autoDb.ensureDbExists(_opCtx);
            WriteUnitOfWork wunit(_opCtx);
            if (_opCtx->writesAreReplicated() &&
                shard_role_details::getRecoveryUnit(_opCtx)->getCommitTimestamp().isNull()) {
                ASSERT_OK(
                    shard_role_details::getRecoveryUnit(_opCtx)->setTimestamp(Timestamp(1, 1)));
            }
            invariant(db->createCollection(_opCtx, nss));
            wunit.commit();
        });
    }

    void insertDocument(const CollectionPtr& coll, const InsertStatement& stmt) {
        // Insert some documents.
        OpDebug* const nullOpDebug = nullptr;
        const bool fromMigrate = false;
        ASSERT_OK(
            collection_internal::insertDocument(_opCtx, coll, stmt, nullOpDebug, fromMigrate));
    }

    void createIndex(CollectionWriter& coll, std::string indexName, const BSONObj& indexKey) {
        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto indexBuildInfo =
            IndexBuildInfo(BSON("v" << 2 << "name" << indexName << "key" << indexKey),
                           *storageEngine,
                           coll->ns().dbName(),
                           VersionContext::getDecoration(_opCtx));

        // Build an index.
        MultiIndexBlock indexer;
        ScopeGuard abortOnExit(
            [&] { indexer.abortIndexBuild(_opCtx, coll, MultiIndexBlock::kNoopOnCleanUpFn); });

        BSONObj indexInfoObj;
        {
            auto swIndexInfoObj =
                indexer.init(_opCtx,
                             coll,
                             {indexBuildInfo},
                             MultiIndexBlock::makeTimestampedIndexOnInitFn(_opCtx, coll.get()));
            ASSERT_OK(swIndexInfoObj.getStatus());
            indexInfoObj = std::move(swIndexInfoObj.getValue()[0]);
        }

        ASSERT_OK(indexer.insertAllDocumentsInCollection(_opCtx, coll->ns()));
        ASSERT_OK(indexer.checkConstraints(_opCtx, coll.get()));

        {
            WriteUnitOfWork wuow(_opCtx);
            // Timestamping index completion. Primaries write an oplog entry.
            ASSERT_OK(indexer.commit(
                _opCtx,
                coll.getWritableCollection(_opCtx),
                [&](const BSONObj& indexSpec, StringData ident) {
                    auto indexBuildInfo = IndexBuildInfo(indexSpec, std::string{ident});
                    indexBuildInfo.setInternalIdents(*storageEngine,
                                                     VersionContext::getDecoration(_opCtx));
                    _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                        _opCtx, coll->ns(), coll->uuid(), indexBuildInfo, false);
                },
                MultiIndexBlock::kNoopOnCommitFn));
            // The timestamping responsibility is placed on the caller rather than the
            // MultiIndexBlock.
            wuow.commit();
        }
        abortOnExit.dismiss();
    }

    std::int32_t itCount(const CollectionPtr& coll) {
        std::uint64_t ret = 0;
        auto cursor =
            coll->getRecordStore()->getCursor(_opCtx, *shard_role_details::getRecoveryUnit(_opCtx));
        while (cursor->next() != boost::none) {
            ++ret;
        }

        return ret;
    }

    BSONObj findOne(const CollectionPtr& coll) {
        auto cursor =
            coll->getRecordStore()->getCursor(_opCtx, *shard_role_details::getRecoveryUnit(_opCtx));
        auto optRecord = cursor->next();
        if (optRecord == boost::none) {
            // Print a stack trace to help disambiguate which `findOne` failed.
            printStackTrace();
            FAIL("Did not find any documents.");
        }
        return optRecord.value().data.getOwned().toBson();
    }

    std::shared_ptr<durable_catalog::CatalogEntryMetaData> getMetaDataAtTime(
        const MDBCatalog* mdbCatalog, const RecordId& catalogId, const Timestamp& ts) {
        OneOffRead oor(_opCtx, ts);
        auto catalogEntry = durable_catalog::getParsedCatalogEntry(_opCtx, catalogId, mdbCatalog);
        if (!catalogEntry) {
            return nullptr;
        }
        return catalogEntry->metadata;
    }

    StatusWith<BSONObj> doApplyOps(const DatabaseName& dbName,
                                   const std::list<BSONObj>& applyOpsList) {
        OneOffRead oor(_opCtx, Timestamp::min());

        BSONObjBuilder result;
        Status status = applyOps(_opCtx,
                                 dbName,
                                 BSON("applyOps" << applyOpsList),
                                 repl::OplogApplication::Mode::kApplyOpsCmd,
                                 &result);
        if (!status.isOK()) {
            return status;
        }

        return {result.obj()};
    }

    BSONObj queryCollection(NamespaceString nss, const BSONObj& query) {
        BSONObj ret;
        ASSERT_TRUE(Helpers::findOne(_opCtx, acquireCollForRead(_opCtx, nss), query, ret))
            << "Query: " << query;
        return ret;
    }

    BSONObj queryOplog(const BSONObj& query) {
        OneOffRead oor(_opCtx, Timestamp::min(), true /* waitForOplog */);
        return queryCollection(NamespaceString::kRsOplogNamespace, query);
    }

    Timestamp getTopOfOplog() {
        OneOffRead oor(_opCtx, Timestamp::min(), true /* waitForOplog */);
        BSONObj ret;
        ASSERT_TRUE(Helpers::getLast(_opCtx, NamespaceString::kRsOplogNamespace, ret));
        return ret["ts"].timestamp();
    }

    void assertMinValidDocumentAtTimestamp(const NamespaceString& nss,
                                           const Timestamp& ts,
                                           const repl::MinValidDocument& expectedDoc) {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const CollectionPtr& collPtr = collAcq.getCollectionPtr();

        OneOffRead oor(_opCtx, ts);

        auto doc =
            repl::MinValidDocument::parse(findOne(collPtr), IDLParserContext("MinValidDocument"));
        ASSERT_EQ(expectedDoc.getMinValidTimestamp(), doc.getMinValidTimestamp())
            << "minValid timestamps weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getMinValidTerm(), doc.getMinValidTerm())
            << "minValid terms weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getAppliedThrough(), doc.getAppliedThrough())
            << "appliedThrough OpTimes weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
        ASSERT_EQ(expectedDoc.getInitialSyncFlag(), doc.getInitialSyncFlag())
            << "Initial sync flags weren't equal at " << ts.toString()
            << ". Expected: " << expectedDoc.toBSON() << ". Found: " << doc.toBSON();
    }

    void assertDocumentAtTimestamp(const CollectionPtr& coll,
                                   const Timestamp& ts,
                                   const BSONObj& expectedDoc) {
        OneOffRead oor(_opCtx, ts);
        if (expectedDoc.isEmpty()) {
            ASSERT_EQ(0, itCount(coll)) << "Should not find any documents in "
                                        << coll->ns().toStringForErrorMsg() << " at ts: " << ts;
        } else {
            ASSERT_EQ(1, itCount(coll)) << "Should find one document in "
                                        << coll->ns().toStringForErrorMsg() << " at ts: " << ts;
            auto doc = findOne(coll);
            ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, expectedDoc))
                << "Doc: " << doc.toString() << " Expected: " << expectedDoc.toString();
        }
    }

    void assertFilteredDocumentAtTimestamp(const CollectionAcquisition& collAcq,
                                           const BSONObj& query,
                                           const Timestamp& ts,
                                           boost::optional<const BSONObj&> expectedDoc) {
        OneOffRead oor(_opCtx, ts);
        BSONObj doc;
        bool found = Helpers::findOne(_opCtx, collAcq, query, doc);
        if (!expectedDoc) {
            ASSERT_FALSE(found) << "Should not find any documents in "
                                << collAcq.nss().toStringForErrorMsg() << " matching " << query
                                << " at ts: " << ts;
        } else {
            ASSERT(found) << "Should find document in " << collAcq.nss().toStringForErrorMsg()
                          << " matching " << query << " at ts: " << ts;
            ASSERT_BSONOBJ_EQ(doc, *expectedDoc);
        }
    }

    void assertOplogDocumentExistsAtTimestamp(const BSONObj& query,
                                              const Timestamp& ts,
                                              bool exists) {
        OneOffRead oor(_opCtx, ts, true);
        BSONObj ret;
        bool found = Helpers::findOne(
            _opCtx, acquireCollForRead(_opCtx, NamespaceString::kRsOplogNamespace), query, ret);
        ASSERT_EQ(found, exists) << "Found " << ret << " at " << ts.toBSON();
        ASSERT_EQ(!ret.isEmpty(), exists) << "Found " << ret << " at " << ts.toBSON();
    }

    void assertOldestActiveTxnTimestampEquals(const boost::optional<Timestamp>& ts,
                                              const Timestamp& atTs) {
        auto oldest = TransactionParticipant::getOldestActiveTimestamp(atTs);
        ASSERT_TRUE(oldest.isOK());
        ASSERT_EQ(oldest.getValue(), ts);
    }

    void assertHasStartOpTime() {
        auto txnDoc = _getTxnDoc();
        ASSERT_TRUE(txnDoc.hasField(SessionTxnRecord::kStartOpTimeFieldName));
    }

    void assertNoStartOpTime() {
        auto txnDoc = _getTxnDoc();
        ASSERT_FALSE(txnDoc.hasField(SessionTxnRecord::kStartOpTimeFieldName));
    }

    void setReplCoordAppliedOpTime(const repl::OpTime& opTime, Date_t wallTime = Date_t()) {
        repl::ReplicationCoordinator::get(_opCtx->getServiceContext())
            ->setMyLastAppliedOpTimeAndWallTimeForward({opTime, wallTime});
        ASSERT_OK(repl::ReplicationCoordinator::get(_opCtx->getServiceContext())
                      ->updateTerm(_opCtx, opTime.getTerm()));
    }

    /**
     * Asserts that the given collection is in (or not in) the MDBCatalog's list of idents at
     * the
     * provided timestamp.
     */
    void assertNamespaceInIdents(NamespaceString nss, Timestamp ts, bool shouldExpect) {
        OneOffRead oor(_opCtx, ts);
        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_IS);

        // getCollectionIdent() returns the ident for the given namespace in the MDBCatalog.
        // getAllIdents() actually looks in the RecordStore for a list of all idents, and is thus
        // versioned by timestamp. We can expect a namespace to have a consistent ident across
        // timestamps, provided the collection does not get renamed.
        auto expectedIdent =
            MDBCatalog::get(_opCtx)->getEntry(autoColl.getCollection()->getCatalogId()).ident;
        auto idents = MDBCatalog::get(_opCtx)->getAllIdents(_opCtx);
        auto found = std::find(idents.begin(), idents.end(), expectedIdent);

        if (shouldExpect) {
            ASSERT(found != idents.end())
                << nss.toStringForErrorMsg() << " was not found at " << ts.toString();
        } else {
            ASSERT(found == idents.end()) << nss.toStringForErrorMsg() << " was found at "
                                          << ts.toString() << " when it should not have been.";
        }
    }

    /**
     * Use `ts` = Timestamp::min to observe all indexes.
     */
    std::string getNewIndexIdentAtTime(MDBCatalog* mdbCatalog,
                                       std::vector<std::string>& origIdents,
                                       Timestamp ts) {
        auto ret = getNewIndexIdentsAtTime(mdbCatalog, origIdents, ts);
        ASSERT_EQ(static_cast<std::size_t>(1), ret.size()) << " Num idents: " << ret.size();
        return ret[0];
    }

    /**
     * Use `ts` = Timestamp::min to observe all indexes.
     */
    std::vector<std::string> getNewIndexIdentsAtTime(MDBCatalog* mdbCatalog,
                                                     std::vector<std::string>& origIdents,
                                                     Timestamp ts) {
        OneOffRead oor(_opCtx, ts);

        // Find the collection and index ident by performing a set difference on the original
        // idents and the current idents.
        std::vector<std::string> identsWithColl = mdbCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> idxIdents;
        std::set_difference(identsWithColl.begin(),
                            identsWithColl.end(),
                            origIdents.begin(),
                            origIdents.end(),
                            std::back_inserter(idxIdents));

        for (const auto& ident : idxIdents) {
            ASSERT(ident.find("index-") == 0) << "Ident is not an index: " << ident;
        }
        return idxIdents;
    }

    std::string getDroppedIndexIdent(MDBCatalog* mdbCatalog, std::vector<std::string>& origIdents) {
        // Find the collection and index ident by performing a set difference on the original
        // idents and the current idents.
        std::vector<std::string> identsWithColl = mdbCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> collAndIdxIdents;
        std::set_difference(origIdents.begin(),
                            origIdents.end(),
                            identsWithColl.begin(),
                            identsWithColl.end(),
                            std::back_inserter(collAndIdxIdents));

        ASSERT(collAndIdxIdents.size() == 1) << "Num idents: " << collAndIdxIdents.size();
        return collAndIdxIdents[0];
    }

    std::vector<std::string> _getIdentDifference(MDBCatalog* mdbCatalog,
                                                 std::vector<std::string>& origIdents) {
        // Find the ident difference by performing a set difference on the original idents and the
        // current idents.
        std::vector<std::string> identsWithColl = mdbCatalog->getAllIdents(_opCtx);
        std::sort(origIdents.begin(), origIdents.end());
        std::sort(identsWithColl.begin(), identsWithColl.end());
        std::vector<std::string> collAndIdxIdents;
        std::set_difference(identsWithColl.begin(),
                            identsWithColl.end(),
                            origIdents.begin(),
                            origIdents.end(),
                            std::back_inserter(collAndIdxIdents));
        return collAndIdxIdents;
    }
    std::tuple<std::string, std::string> getNewCollectionIndexIdent(
        MDBCatalog* mdbCatalog, std::vector<std::string>& origIdents) {
        // Find the collection and index ident difference.
        auto collAndIdxIdents = _getIdentDifference(mdbCatalog, origIdents);

        ASSERT(collAndIdxIdents.size() == 1 || collAndIdxIdents.size() == 2);
        if (collAndIdxIdents.size() == 1) {
            // `system.profile` collections do not have an `_id` index.
            return std::tie(collAndIdxIdents[0], "");
        }
        if (collAndIdxIdents.size() == 2) {
            // The idents are sorted, so the `collection-...` comes before `index-...`
            return std::tie(collAndIdxIdents[0], collAndIdxIdents[1]);
        }

        MONGO_UNREACHABLE;
    }

    /**
     * Note: expectedNewIndexIdents should include the _id index.
     */
    void assertRenamedCollectionIdentsAtTimestamp(MDBCatalog* mdbCatalog,
                                                  std::vector<std::string>& origIdents,
                                                  size_t expectedNewIndexIdents,
                                                  Timestamp timestamp) {
        OneOffRead oor(_opCtx, timestamp);
        // Find the collection and index ident difference.
        auto collAndIdxIdents = _getIdentDifference(mdbCatalog, origIdents);
        size_t newNssIdents, newIdxIdents;
        newNssIdents = newIdxIdents = 0;
        for (const auto& ident : collAndIdxIdents) {
            ASSERT(ident.find("index-") == 0 || ident.find("collection-") == 0)
                << "Ident is not an index or collection: " << ident;
            if (ident.find("collection-") == 0) {
                ASSERT(++newNssIdents == 1) << "Expected new collection idents (1) differ from "
                                               "actual new collection idents ("
                                            << newNssIdents << ")";
            } else {
                newIdxIdents++;
            }
        }
        ASSERT(expectedNewIndexIdents == newIdxIdents)
            << "Expected new index idents (" << expectedNewIndexIdents
            << ") differ from actual new index idents (" << newIdxIdents << ")";
    }

    void assertIdentsExistAtTimestamp(MDBCatalog* mdbCatalog,
                                      StringData collIdent,
                                      StringData indexIdent,
                                      Timestamp timestamp) {
        OneOffRead oor(_opCtx, timestamp);

        auto allIdents = mdbCatalog->getAllIdents(_opCtx);
        if (collIdent.size() > 0) {
            // Index build test does not pass in a collection ident.
            ASSERT(std::find(allIdents.begin(), allIdents.end(), collIdent) != allIdents.end());
        }

        if (indexIdent.size() > 0) {
            // `system.profile` does not have an `_id` index.
            ASSERT(std::find(allIdents.begin(), allIdents.end(), indexIdent) != allIdents.end());
        }
    }

    void assertIdentsMissingAtTimestamp(MDBCatalog* mdbCatalog,
                                        StringData collIdent,
                                        StringData indexIdent,
                                        Timestamp timestamp) {
        OneOffRead oor(_opCtx, timestamp);
        auto allIdents = mdbCatalog->getAllIdents(_opCtx);
        if (collIdent.size() > 0) {
            // Index build test does not pass in a collection ident.
            ASSERT(std::find(allIdents.begin(), allIdents.end(), collIdent) == allIdents.end());
        }

        ASSERT(std::find(allIdents.begin(), allIdents.end(), indexIdent) == allIdents.end())
            << "Ident: " << indexIdent << " Timestamp: " << timestamp;
    }

    std::string dumpMultikeyPaths(const MultikeyPaths& multikeyPaths) {
        std::stringstream ss;

        ss << "[ ";
        for (const auto& multikeyComponents : multikeyPaths) {
            ss << "[ ";
            for (const auto& multikeyComponent : multikeyComponents) {
                ss << multikeyComponent << " ";
            }
            ss << "] ";
        }
        ss << "]";

        return ss.str();
    }

    void assertMultikeyPaths(OperationContext* _opCtx,
                             const CollectionPtr& collection,
                             StringData indexName,
                             Timestamp ts,
                             bool shouldBeMultikey,
                             const MultikeyPaths& expectedMultikeyPaths) {
        MDBCatalog* mdbCatalog = MDBCatalog::get(_opCtx);
        auto indexMetaData = getIndexMetaData(
            getMetaDataAtTime(mdbCatalog, collection->getCatalogId(), ts), indexName);
        if (!shouldBeMultikey) {
            ASSERT_FALSE(indexMetaData.multikey)
                << "index " << indexName << " should not be multikey at timestamp " << ts;
        } else {
            ASSERT_TRUE(indexMetaData.multikey)
                << "index " << indexName << " should be multikey at timestamp " << ts;
        }

        MultikeyPaths actualMultikeyPaths = indexMetaData.multikeyPaths;
        const bool match = (expectedMultikeyPaths == actualMultikeyPaths);
        if (!match) {
            FAIL(std::string(str::stream()
                             << "TS: " << ts.toString()
                             << ", Expected: " << dumpMultikeyPaths(expectedMultikeyPaths)
                             << ", Actual: " << dumpMultikeyPaths(actualMultikeyPaths)));
        }
        ASSERT_TRUE(match);
    }

private:
    BSONObj _getTxnDoc() {
        auto txnParticipant = TransactionParticipant::get(_opCtx);
        auto txnsFilter =
            BSON("_id" << _opCtx->getLogicalSessionId()->toBSON() << "txnNum"
                       << txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber());
        return queryCollection(NamespaceString::kSessionTransactionsTableNamespace, txnsFilter);
    }
};

TEST_F(StorageTimestampTest, SecondaryInsertTimes) {
    Lock::GlobalWrite lk{_opCtx};  // avoid global lock upgrade during applyOps.

    // In order for applyOps to assign timestamps, we must be in non-replicated mode.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    // Create a new collection.
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampedUpdates");
    create(nss);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    const std::int32_t docsToInsert = 10;
    const LogicalTime firstInsertTime = _clock->tickClusterTime(docsToInsert);
    for (std::int32_t idx = 0; idx < docsToInsert; ++idx) {
        BSONObjBuilder result;
        ASSERT_OK(applyOps(_opCtx,
                           nss.dbName(),
                           BSON("applyOps" << BSON_ARRAY(
                                    BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp() << "t"
                                              << 1LL << "v" << 2 << "op"
                                              << "i"
                                              << "ns" << nss.ns_forTest() << "ui"
                                              << collAcq.getCollectionPtr()->uuid() << "wall"
                                              << Date_t() << "o" << BSON("_id" << idx))
                                    << BSON("ts" << firstInsertTime.addTicks(idx).asTimestamp()
                                                 << "t" << 1LL << "op"
                                                 << "c"
                                                 << "ns"
                                                 << "test.$cmd"
                                                 << "wall" << Date_t() << "o"
                                                 << BSON("applyOps" << BSONArrayBuilder().arr())))),
                           repl::OplogApplication::Mode::kApplyOpsCmd,
                           &result));
    }

    for (std::int32_t idx = 0; idx < docsToInsert; ++idx) {
        OneOffRead oor(_opCtx, firstInsertTime.addTicks(idx).asTimestamp());

        BSONObj result;
        ASSERT(Helpers::getLast(_opCtx, nss, result)) << " idx is " << idx;
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
            << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
    }
}

TEST_F(StorageTimestampTest, SecondaryArrayInsertTimes) {
    // In order for oplog application to assign timestamps, we must be in non-replicated mode
    // and disable document validation.
    repl::UnreplicatedWritesBlock uwb(_opCtx);
    DisableDocumentValidation validationDisabler(_opCtx);

    // Create a new collection.
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampedUpdates");
    create(nss);

    const std::int32_t docsToInsert = 10;
    const LogicalTime firstInsertTime = _clock->tickClusterTime(docsToInsert);
    BSONObjBuilder oplogCommonBuilder;

    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        oplogCommonBuilder << "v" << 2 << "op"
                           << "i"
                           << "ns" << nss.ns_forTest() << "ui" << collAcq.getCollectionPtr()->uuid()
                           << "wall" << Date_t();
    }
    auto oplogCommon = oplogCommonBuilder.done();

    std::vector<repl::OplogEntry> oplogEntries;
    oplogEntries.reserve(docsToInsert);
    std::vector<repl::ApplierOperation> opPtrs;
    BSONObjBuilder oplogEntryBuilders[docsToInsert];
    for (std::int32_t idx = 0; idx < docsToInsert; ++idx) {
        auto o = BSON("_id" << idx);
        // Populate the "ts" field.
        oplogEntryBuilders[idx] << "ts" << firstInsertTime.addTicks(idx).asTimestamp();
        // Populate the "t" (term) field.
        oplogEntryBuilders[idx] << "t" << 1LL;
        // Populate the "o" field.
        oplogEntryBuilders[idx] << "o" << o;
        // Populate the "wall" field
        oplogEntryBuilders[idx] << "wall" << Date_t();
        // Populate the other common fields.
        oplogEntryBuilders[idx].appendElementsUnique(oplogCommon);
        // Insert ops to be applied.
        oplogEntries.push_back(repl::OplogEntry(oplogEntryBuilders[idx].done()));
        opPtrs.emplace_back(&(oplogEntries.back()));
    }

    repl::OplogEntryOrGroupedInserts groupedInserts(opPtrs.cbegin(), opPtrs.cend());
    const bool dataIsConsistent = true;
    ASSERT_OK(repl::applyOplogEntryOrGroupedInserts(
        _opCtx, groupedInserts, repl::OplogApplication::Mode::kSecondary, dataIsConsistent));

    for (std::int32_t idx = 0; idx < docsToInsert; ++idx) {
        OneOffRead oor(_opCtx, firstInsertTime.addTicks(idx).asTimestamp());

        BSONObj result;
        ASSERT(Helpers::getLast(_opCtx, nss, result)) << " idx is " << idx;
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(result, BSON("_id" << idx)))
            << "Doc: " << result.toString() << " Expected: " << BSON("_id" << idx);
    }
}


TEST_F(StorageTimestampTest, SecondaryDeleteTimes) {
    // In order for applyOps to assign timestamps, we must be in non-replicated mode.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    // Create a new collection.
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampedDeletes");
    create(nss);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    // Insert some documents.
    const std::int32_t docsToInsert = 10;
    const LogicalTime firstInsertTime = _clock->tickClusterTime(docsToInsert);
    const LogicalTime lastInsertTime = firstInsertTime.addTicks(docsToInsert - 1);
    WriteUnitOfWork wunit(_opCtx);
    for (std::int32_t num = 0; num < docsToInsert; ++num) {
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(BSON("_id" << num << "a" << num),
                                       firstInsertTime.addTicks(num).asTimestamp(),
                                       0LL));
    }
    wunit.commit();
    ASSERT_EQ(docsToInsert, itCount(collAcq.getCollectionPtr()));

    // Delete all documents one at a time.
    const LogicalTime startDeleteTime = _clock->tickClusterTime(docsToInsert);
    for (std::int32_t num = 0; num < docsToInsert; ++num) {
        ASSERT_OK(doApplyOps(nss.dbName(),
                             {BSON("ts" << startDeleteTime.addTicks(num).asTimestamp() << "t" << 0LL
                                        << "v" << 2 << "op"
                                        << "d"
                                        << "ns" << nss.ns_forTest() << "ui"
                                        << collAcq.getCollectionPtr()->uuid() << "wall" << Date_t()
                                        << "o" << BSON("_id" << num))})
                      .getStatus());
    }

    for (std::int32_t num = 0; num <= docsToInsert; ++num) {
        // The first loop queries at `lastInsertTime` and should count all documents. Querying
        // at each successive tick counts one less document.
        OneOffRead oor(_opCtx, lastInsertTime.addTicks(num).asTimestamp());
        ASSERT_EQ(docsToInsert - num, itCount(collAcq.getCollectionPtr()));
    }
}

TEST_F(StorageTimestampTest, SecondaryUpdateTimes) {
    // In order for applyOps to assign timestamps, we must be in non-replicated mode.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    // Create a new collection.
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampedUpdates");
    create(nss);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    // Insert one document that will go through a series of updates.
    const LogicalTime insertTime = _clock->tickClusterTime(1);
    WriteUnitOfWork wunit(_opCtx);
    insertDocument(collAcq.getCollectionPtr(),
                   InsertStatement(BSON("_id" << 0), insertTime.asTimestamp(), 0LL));
    wunit.commit();
    ASSERT_EQ(1, itCount(collAcq.getCollectionPtr()));

    // Each pair in the vector represents the update to perform at the next tick of the
    // clock. `pair.first` is the update to perform and `pair.second` is the full value of the
    // document after the transformation.
    const std::vector<std::pair<BSONObj, BSONObj>> updates = {
        {update_oplog_entry::makeDeltaOplogEntry(
             BSON(doc_diff::kUpdateSectionFieldName << fromjson("{val: 1}"))),
         BSON("_id" << 0 << "val" << 1)},
        {update_oplog_entry::makeDeltaOplogEntry(
             BSON(doc_diff::kDeleteSectionFieldName << fromjson("{val: false}"))),
         BSON("_id" << 0)},
        {update_oplog_entry::makeDeltaOplogEntry(
             BSON(doc_diff::kUpdateSectionFieldName << fromjson("{theSet: [1]}"))),
         BSON("_id" << 0 << "theSet" << BSON_ARRAY(1))},
        {update_oplog_entry::makeDeltaOplogEntry(BSON(
             "stheSet" << BSON(doc_diff::kArrayHeader << true << doc_diff::kResizeSectionFieldName
                                                      << 2 << "u1" << 2))),
         BSON("_id" << 0 << "theSet" << BSON_ARRAY(1 << 2))},
        {update_oplog_entry::makeDeltaOplogEntry(BSON(
             "stheSet" << BSON(doc_diff::kArrayHeader << true << doc_diff::kResizeSectionFieldName
                                                      << 1 << "u0" << 2))),
         BSON("_id" << 0 << "theSet" << BSON_ARRAY(2))},
        {update_oplog_entry::makeDeltaOplogEntry(
             BSON("stheSet" << BSON(doc_diff::kArrayHeader
                                    << true << doc_diff::kResizeSectionFieldName << 0))),
         BSON("_id" << 0 << "theSet" << BSONArray())},
        {update_oplog_entry::makeDeltaOplogEntry(
             BSON(doc_diff::kUpdateSectionFieldName << fromjson("{theMap: {val: 1}}"))),
         BSON("_id" << 0 << "theSet" << BSONArray() << "theMap" << BSON("val" << 1))},
        {update_oplog_entry::makeDeltaOplogEntry(BSON(doc_diff::kDeleteSectionFieldName
                                                      << fromjson("{theSet: false}")
                                                      << doc_diff::kUpdateSectionFieldName
                                                      << fromjson("{theOtherSet: []}"))),
         BSON("_id" << 0 << "theMap" << BSON("val" << 1) << "theOtherSet" << BSONArray())}};

    const LogicalTime firstUpdateTime = _clock->tickClusterTime(updates.size());
    for (std::size_t idx = 0; idx < updates.size(); ++idx) {
        ASSERT_OK(doApplyOps(nss.dbName(),
                             {BSON("ts" << firstUpdateTime.addTicks(idx).asTimestamp() << "t" << 0LL
                                        << "v" << 2 << "op"
                                        << "u"
                                        << "ns" << nss.ns_forTest() << "ui"
                                        << collAcq.getCollectionPtr()->uuid() << "wall" << Date_t()
                                        << "o2" << BSON("_id" << 0) << "o" << updates[idx].first)})
                      .getStatus());
    }

    for (std::size_t idx = 0; idx < updates.size(); ++idx) {
        // Querying at each successive ticks after `insertTime` sees the document transform in
        // the series.
        OneOffRead oor(_opCtx, insertTime.addTicks(idx + 1).asTimestamp());

        auto doc = findOne(collAcq.getCollectionPtr());
        ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, updates[idx].second))
            << "Doc: " << doc.toString() << " Expected: " << updates[idx].second.toString();
    }
}

TEST_F(StorageTimestampTest, SecondaryInsertToUpsert) {
    // In order for applyOps to assign timestamps, we must be in non-replicated mode.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    // Create a new collection.
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.insertToUpsert");
    create(nss);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    const LogicalTime insertTime = _clock->tickClusterTime(2);

    // This applyOps runs into an insert of `{_id: 0, field: 0}` followed by a second insert
    // on the same collection with `{_id: 0}`. It's expected for this second insert to be
    // turned into an upsert. The goal document does not contain `field: 0`.
    BSONObjBuilder resultBuilder;
    auto result = unittest::assertGet(doApplyOps(
        nss.dbName(),
        {BSON("ts" << insertTime.asTimestamp() << "t" << 1LL << "op"
                   << "i"
                   << "ns" << nss.ns_forTest() << "ui" << collAcq.getCollectionPtr()->uuid()
                   << "wall" << Date_t() << "o" << BSON("_id" << 0 << "field" << 0)),
         BSON("ts" << insertTime.addTicks(1).asTimestamp() << "t" << 1LL << "op"
                   << "i"
                   << "ns" << nss.ns_forTest() << "ui" << collAcq.getCollectionPtr()->uuid()
                   << "wall" << Date_t() << "o" << BSON("_id" << 0))}));

    ASSERT_EQ(2, result.getIntField("applied"));
    ASSERT(result["results"].Array()[0].Bool());
    ASSERT(result["results"].Array()[1].Bool());

    // Reading at `insertTime` should show the original document, `{_id: 0, field: 0}`.
    auto recoveryUnit = shard_role_details::getRecoveryUnit(_opCtx);
    recoveryUnit->abandonSnapshot();
    recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                         insertTime.asTimestamp());
    auto doc = findOne(collAcq.getCollectionPtr());
    ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0 << "field" << 0)))
        << "Doc: " << doc.toString() << " Expected: {_id: 0, field: 0}";

    // Reading at `insertTime + 1` should show the second insert that got converted to an
    // upsert, `{_id: 0}`.
    recoveryUnit->abandonSnapshot();
    recoveryUnit->setTimestampReadSource(RecoveryUnit::ReadSource::kProvided,
                                         insertTime.addTicks(1).asTimestamp());
    doc = findOne(collAcq.getCollectionPtr());
    ASSERT_EQ(0, SimpleBSONObjComparator::kInstance.compare(doc, BSON("_id" << 0)))
        << "Doc: " << doc.toString() << " Expected: {_id: 0}";
}

TEST_F(StorageTimestampTest, SecondaryCreateCollection) {
    // In order for applyOps to assign timestamps, we must be in non-replicated mode.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.secondaryCreateCollection");
    ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss));

    ASSERT_FALSE(acquireCollForRead(_opCtx, nss).exists());

    BSONObjBuilder resultBuilder;
    auto swResult =
        doApplyOps(nss.dbName(),
                   {
                       BSON("ts" << _presentTs << "t" << 1LL << "op"
                                 << "c"
                                 << "ui" << UUID::gen() << "ns" << nss.getCommandNS().ns_forTest()
                                 << "wall" << Date_t() << "o" << BSON("create" << nss.coll())),
                   });
    ASSERT_OK(swResult);

    ASSERT(acquireCollForRead(_opCtx, nss).exists());

    assertNamespaceInIdents(nss, _pastTs, false);
    assertNamespaceInIdents(nss, _presentTs, true);
    assertNamespaceInIdents(nss, _futureTs, true);
    assertNamespaceInIdents(nss, _nullTs, true);
}

TEST_F(StorageTimestampTest, SecondaryCreateTwoCollections) {
    // In order for applyOps to assign timestamps, we must be in non-replicated mode.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    std::string dbName = "unittest";
    NamespaceString nss1 =
        NamespaceString::createNamespaceString_forTest(dbName, "secondaryCreateTwoCollections1");
    NamespaceString nss2 =
        NamespaceString::createNamespaceString_forTest(dbName, "secondaryCreateTwoCollections2");
    ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss1));
    ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss2));

    ASSERT_FALSE(acquireCollForRead(_opCtx, nss1).exists());
    ASSERT_FALSE(acquireCollForRead(_opCtx, nss2).exists());

    const LogicalTime dummyLt = const_cast<const LogicalTime&>(_futureLt).addTicks(1);
    const Timestamp dummyTs = dummyLt.asTimestamp();

    BSONObjBuilder resultBuilder;
    auto swResult =
        doApplyOps(DatabaseName::createDatabaseName_forTest(boost::none, dbName),
                   {
                       BSON("ts" << _presentTs << "t" << 1LL << "op"
                                 << "c"
                                 << "ui" << UUID::gen() << "ns" << nss1.getCommandNS().ns_forTest()
                                 << "wall" << Date_t() << "o" << BSON("create" << nss1.coll())),
                       BSON("ts" << _futureTs << "t" << 1LL << "op"
                                 << "c"
                                 << "ui" << UUID::gen() << "ns" << nss2.getCommandNS().ns_forTest()
                                 << "wall" << Date_t() << "o" << BSON("create" << nss2.coll())),
                   });
    ASSERT_OK(swResult);

    ASSERT(acquireCollForRead(_opCtx, nss1).exists());
    ASSERT(acquireCollForRead(_opCtx, nss2).exists());

    assertNamespaceInIdents(nss1, _pastTs, false);
    assertNamespaceInIdents(nss1, _presentTs, true);
    assertNamespaceInIdents(nss1, _futureTs, true);
    assertNamespaceInIdents(nss1, dummyTs, true);
    assertNamespaceInIdents(nss1, _nullTs, true);

    assertNamespaceInIdents(nss2, _pastTs, false);
    assertNamespaceInIdents(nss2, _presentTs, false);
    assertNamespaceInIdents(nss2, _futureTs, true);
    assertNamespaceInIdents(nss2, dummyTs, true);
    assertNamespaceInIdents(nss2, _nullTs, true);
}

TEST_F(StorageTimestampTest, SecondaryCreateCollectionBetweenInserts) {
    // In order for applyOps to assign timestamps, we must be in non-replicated mode.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    std::string dbName = "unittest";
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(
        dbName, "secondaryCreateCollectionBetweenInserts1");
    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(
        dbName, "secondaryCreateCollectionBetweenInserts2");
    BSONObj doc1 = BSON("_id" << 1 << "field" << 1);
    BSONObj doc2 = BSON("_id" << 2 << "field" << 2);

    const UUID uuid2 = UUID::gen();

    const LogicalTime insert2Lt = const_cast<const LogicalTime&>(_futureLt).addTicks(1);
    const Timestamp insert2Ts = insert2Lt.asTimestamp();

    const LogicalTime dummyLt = insert2Lt.addTicks(1);
    const Timestamp dummyTs = dummyLt.asTimestamp();

    {
        create(nss1);
        Lock::GlobalWrite lk{_opCtx};  // avoid global lock upgrade during applyOps.
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss1, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss2));
        ASSERT_FALSE(acquireCollForRead(_opCtx, nss2).exists());

        BSONObjBuilder resultBuilder;
        auto swResult = doApplyOps(
            DatabaseName::createDatabaseName_forTest(boost::none, dbName),
            {
                BSON("ts" << _presentTs << "t" << 1LL << "op"
                          << "i"
                          << "ns" << nss1.ns_forTest() << "ui" << collAcq.getCollectionPtr()->uuid()
                          << "wall" << Date_t() << "o" << doc1),
                BSON("ts" << _futureTs << "t" << 1LL << "op"
                          << "c"
                          << "ui" << uuid2 << "ns" << nss2.getCommandNS().ns_forTest() << "wall"
                          << Date_t() << "o" << BSON("create" << nss2.coll())),
                BSON("ts" << insert2Ts << "t" << 1LL << "op"
                          << "i"
                          << "ns" << nss2.ns_forTest() << "ui" << uuid2 << "wall" << Date_t() << "o"
                          << doc2),
            });
        ASSERT_OK(swResult);
    }

    {
        auto autoColl1 = acquireCollForRead(_opCtx, nss1);
        const auto& coll1 = autoColl1.getCollectionPtr();
        ASSERT(coll1);
        auto autoColl2 = acquireCollForRead(_opCtx, nss2);
        const auto& coll2 = autoColl2.getCollectionPtr();
        ASSERT(coll2);

        assertDocumentAtTimestamp(coll1, _pastTs, BSONObj());
        assertDocumentAtTimestamp(coll1, _presentTs, doc1);
        assertDocumentAtTimestamp(coll1, _futureTs, doc1);
        assertDocumentAtTimestamp(coll1, insert2Ts, doc1);
        assertDocumentAtTimestamp(coll1, dummyTs, doc1);
        assertDocumentAtTimestamp(coll1, _nullTs, doc1);

        assertNamespaceInIdents(nss2, _pastTs, false);
        assertNamespaceInIdents(nss2, _presentTs, false);
        assertNamespaceInIdents(nss2, _futureTs, true);
        assertNamespaceInIdents(nss2, insert2Ts, true);
        assertNamespaceInIdents(nss2, dummyTs, true);
        assertNamespaceInIdents(nss2, _nullTs, true);

        assertDocumentAtTimestamp(coll2, _pastTs, BSONObj());
        assertDocumentAtTimestamp(coll2, _presentTs, BSONObj());
        assertDocumentAtTimestamp(coll2, _futureTs, BSONObj());
        assertDocumentAtTimestamp(coll2, insert2Ts, doc2);
        assertDocumentAtTimestamp(coll2, dummyTs, doc2);
        assertDocumentAtTimestamp(coll2, _nullTs, doc2);
    }
}

TEST_F(StorageTimestampTest, PrimaryCreateCollectionInApplyOps) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.primaryCreateCollectionInApplyOps");
    ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(_opCtx, nss));

    ASSERT_FALSE(acquireCollForRead(_opCtx, nss).exists());

    BSONObjBuilder resultBuilder;
    auto swResult =
        doApplyOps(nss.dbName(),
                   {
                       BSON("ts" << _presentTs << "t" << 1LL << "op"
                                 << "c"
                                 << "ui" << UUID::gen() << "ns" << nss.getCommandNS().ns_forTest()
                                 << "wall" << Date_t() << "o" << BSON("create" << nss.coll())),
                   });
    ASSERT_OK(swResult);

    ASSERT(acquireCollForRead(_opCtx, nss).exists());

    BSONObj result;
    ASSERT(Helpers::getLast(_opCtx, NamespaceString::kRsOplogNamespace, result));
    repl::OplogEntry op(result);
    ASSERT(op.getOpType() == repl::OpTypeEnum::kCommand) << op.toBSONForLogging();
    // The next logOp() call will get 'futureTs', which will be the timestamp at which we do
    // the write. Thus we expect the write to appear at 'futureTs' and not before.
    ASSERT_EQ(op.getTimestamp(), _futureTs) << op.toBSONForLogging();
    ASSERT_EQ(op.getNss().ns_forTest(), nss.getCommandNS().ns_forTest()) << op.toBSONForLogging();
    ASSERT_BSONOBJ_EQ(op.getObject(), BSON("create" << nss.coll()));

    assertNamespaceInIdents(nss, _pastTs, false);
    assertNamespaceInIdents(nss, _presentTs, false);
    assertNamespaceInIdents(nss, _futureTs, true);
    assertNamespaceInIdents(nss, _nullTs, true);
}

TEST_F(StorageTimestampTest, SecondarySetIndexMultikeyOnInsert) {
    // Pretend to be a secondary.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.SecondarySetIndexMultikeyOnInsert");
    create(nss);
    UUID uuid = UUID::gen();
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        uuid = collAcq.getCollectionPtr()->uuid();
    }
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                 << static_cast<int>(kIndexVersion));
    ASSERT_OK(createIndexFromSpec(_opCtx, _clock, nss.ns_forTest(), indexSpec));

    _coordinatorMock->alwaysAllowWrites(false);

    const LogicalTime pastTime = _clock->tickClusterTime(1);
    const LogicalTime insertTime0 = _clock->tickClusterTime(1);
    const LogicalTime insertTime1 = _clock->tickClusterTime(1);
    const LogicalTime insertTime2 = _clock->tickClusterTime(1);

    BSONObj doc0 = BSON("_id" << 0 << "a" << 3);
    BSONObj doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
    BSONObj doc2 = BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2));
    auto op0 = repl::OplogEntry(BSON(
        "ts" << insertTime0.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
             << "i"
             << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t() << "o" << doc0));
    auto op1 = repl::OplogEntry(BSON(
        "ts" << insertTime1.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
             << "i"
             << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t() << "o" << doc1));
    auto op2 = repl::OplogEntry(BSON(
        "ts" << insertTime2.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
             << "i"
             << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t() << "o" << doc2));
    std::vector<repl::OplogEntry> ops = {op0, op1, op2};

    DoNothingOplogApplierObserver observer;
    auto storageInterface = repl::StorageInterface::get(_opCtx);
    auto workerPool = repl::makeReplWorkerPool();
    repl::OplogApplierImpl oplogApplier(
        nullptr,  // task executor. not required for applyOplogBatch().
        nullptr,  // oplog buffer. not required for applyOplogBatch().
        &observer,
        _coordinatorMock,
        _consistencyMarkers,
        storageInterface,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kSecondary),
        workerPool.get());
    ASSERT_EQUALS(op2.getOpTime(), unittest::assertGet(oplogApplier.applyOplogBatch(_opCtx, ops)));

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, pastTime.asTimestamp(), false, {{}});
    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, insertTime0.asTimestamp(), true, {{0}});
    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, insertTime1.asTimestamp(), true, {{0}});
    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, insertTime2.asTimestamp(), true, {{0}});
}

TEST_F(StorageTimestampTest, SecondarySetWildcardIndexMultikeyOnInsert) {
    // Pretend to be a secondary.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.SecondarySetWildcardIndexMultikeyOnInsert");
    // Use a capped collection to prevent the batch applier from grouping insert operations
    // together in the same WUOW. This test attempts to apply operations out of order, but the
    // storage engine does not allow an operation to set out-of-order timestamps in the same
    // WUOW.
    ASSERT_OK(createCollection(
        _opCtx,
        nss.dbName(),
        BSON("create" << nss.coll() << "capped" << true << "size" << 1 * 1024 * 1024)));
    auto uuid = [&]() {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        return collAcq.getCollectionPtr()->uuid();
    }();

    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("$**" << 1) << "v"
                                 << static_cast<int>(kIndexVersion));
    ASSERT_OK(createIndexFromSpec(_opCtx, _clock, nss.ns_forTest(), indexSpec));

    _coordinatorMock->alwaysAllowWrites(false);

    const LogicalTime insertTime0 = _clock->tickClusterTime(1);
    const LogicalTime insertTime1 = _clock->tickClusterTime(1);
    const LogicalTime insertTime2 = _clock->tickClusterTime(1);

    BSONObj doc0 = BSON("_id" << 0 << "a" << 3);
    BSONObj doc1 = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
    BSONObj doc2 = BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2));
    auto op0 = repl::OplogEntry(BSON(
        "ts" << insertTime0.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
             << "i"
             << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t() << "o" << doc0));
    auto op1 = repl::OplogEntry(BSON(
        "ts" << insertTime1.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
             << "i"
             << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t() << "o" << doc1));
    auto op2 = repl::OplogEntry(BSON(
        "ts" << insertTime2.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
             << "i"
             << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t() << "o" << doc2));

    // Coerce oplog application to apply op2 before op1. This does not guarantee the actual
    // order of application however, because the oplog applier applies these operations in
    // parallel across several threads. The test accepts the possibility of a false negative
    // (test passes when it should fail) in favor of occasionally finding a true positive (test
    // fails as intended).
    std::vector<repl::OplogEntry> ops = {op0, op2, op1};

    DoNothingOplogApplierObserver observer;
    auto storageInterface = repl::StorageInterface::get(_opCtx);
    auto workerPool = repl::makeReplWorkerPool();
    repl::OplogApplierImpl oplogApplier(
        nullptr,  // task executor. not required for applyOplogBatch().
        nullptr,  // oplog buffer. not required for applyOplogBatch().
        &observer,
        _coordinatorMock,
        _consistencyMarkers,
        storageInterface,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kStableRecovering),
        workerPool.get());

    uassertStatusOK(oplogApplier.applyOplogBatch(_opCtx, ops));

    const auto collAcq = acquireCollForRead(_opCtx, nss);
    auto wildcardIndexDescriptor =
        collAcq.getCollectionPtr()->getIndexCatalog()->findIndexByName(_opCtx, indexName);
    const IndexCatalogEntry* entry =
        collAcq.getCollectionPtr()->getIndexCatalog()->getEntry(wildcardIndexDescriptor);
    {
        // Verify that, even though op2 was applied first, the multikey state is observed in all
        // WiredTiger transactions that can contain the data written by op1.
        OneOffRead oor(_opCtx, insertTime1.asTimestamp());
        MultikeyMetadataAccessStats stats;
        std::set<FieldRef> paths = getWildcardMultikeyPathSet(_opCtx, entry, &stats);
        ASSERT_EQUALS(1, paths.size());
        ASSERT_EQUALS("a", paths.begin()->dottedField());
    }
    {
        // Oplog application conservatively uses the first optime in the batch, insertTime0, as
        // the point at which the index became multikey, despite the fact that the earliest op
        // which caused the index to become multikey did not occur until insertTime1. This works
        // because if we construct a query plan that incorrectly believes a particular path to
        // be multikey, the plan will still be correct (if possibly sub-optimal). Conversely, if
        // we were to construct a query plan that incorrectly believes a path is NOT multikey,
        // it could produce incorrect results.
        OneOffRead oor(_opCtx, insertTime0.asTimestamp());
        MultikeyMetadataAccessStats stats;
        std::set<FieldRef> paths = getWildcardMultikeyPathSet(_opCtx, entry, &stats);
        ASSERT_EQUALS(1, paths.size());
        ASSERT_EQUALS("a", paths.begin()->dottedField());
    }
}

TEST_F(StorageTimestampTest, SecondarySetWildcardIndexMultikeyOnUpdate) {
    // Pretend to be a secondary.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.SecondarySetWildcardIndexMultikeyOnUpdate");
    create(nss);
    UUID uuid = UUID::gen();
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        uuid = collAcq.getCollectionPtr()->uuid();
    }
    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("$**" << 1) << "v"
                                 << static_cast<int>(kIndexVersion));
    ASSERT_OK(createIndexFromSpec(_opCtx, _clock, nss.ns_forTest(), indexSpec));

    _coordinatorMock->alwaysAllowWrites(false);

    const LogicalTime insertTime0 = _clock->tickClusterTime(1);
    const LogicalTime updateTime1 = _clock->tickClusterTime(1);
    const LogicalTime updateTime2 = _clock->tickClusterTime(1);

    BSONObj doc0 = fromjson("{_id: 0, a: 3}");
    BSONObj doc1 = fromjson("{$v: 2, diff: {u: {a: [1,2]}}}");
    BSONObj doc2 = fromjson("{$v: 2, diff: {u: {a: [1,2]}}}");
    auto op0 = repl::OplogEntry(BSON(
        "ts" << insertTime0.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
             << "i"
             << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t() << "o" << doc0));
    auto op1 =
        repl::OplogEntry(BSON("ts" << updateTime1.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
                                   << "u"
                                   << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t()
                                   << "o" << doc1 << "o2" << BSON("_id" << 0)));
    auto op2 =
        repl::OplogEntry(BSON("ts" << updateTime2.asTimestamp() << "t" << 1LL << "v" << 2 << "op"
                                   << "u"
                                   << "ns" << nss.ns_forTest() << "ui" << uuid << "wall" << Date_t()
                                   << "o" << doc2 << "o2" << BSON("_id" << 0)));

    // Coerce oplog application to apply op2 before op1. This does not guarantee the actual
    // order of application however, because the oplog applier applies these operations in
    // parallel across several threads. The test accepts the possibility of a false negative
    // (test passes when it should fail) in favor of occasionally finding a true positive (test
    // fails as intended).
    std::vector<repl::OplogEntry> ops = {op0, op2, op1};

    DoNothingOplogApplierObserver observer;
    auto storageInterface = repl::StorageInterface::get(_opCtx);
    auto workerPool = repl::makeReplWorkerPool();
    repl::OplogApplierImpl oplogApplier(
        nullptr,  // task executor. not required for applyOplogBatch().
        nullptr,  // oplog buffer. not required for applyOplogBatch().
        &observer,
        _coordinatorMock,
        _consistencyMarkers,
        storageInterface,
        repl::OplogApplier::Options(repl::OplogApplication::Mode::kStableRecovering),
        workerPool.get());

    uassertStatusOK(oplogApplier.applyOplogBatch(_opCtx, ops));

    const auto collAcq = acquireCollForRead(_opCtx, nss);
    auto wildcardIndexDescriptor =
        collAcq.getCollectionPtr()->getIndexCatalog()->findIndexByName(_opCtx, indexName);
    const IndexCatalogEntry* entry =
        collAcq.getCollectionPtr()->getIndexCatalog()->getEntry(wildcardIndexDescriptor);
    {
        // Verify that, even though op2 was applied first, the multikey state is observed in all
        // WiredTiger transactions that can contain the data written by op1.
        OneOffRead oor(_opCtx, updateTime1.asTimestamp());
        MultikeyMetadataAccessStats stats;
        std::set<FieldRef> paths = getWildcardMultikeyPathSet(_opCtx, entry, &stats);
        ASSERT_EQUALS(1, paths.size());
        ASSERT_EQUALS("a", paths.begin()->dottedField());
    }
    {
        // Oplog application conservatively uses the first optime in the batch, insertTime0, as
        // the point at which the index became multikey, despite the fact that the earliest op
        // which caused the index to become multikey did not occur until updateTime1. This works
        // because if we construct a query plan that incorrectly believes a particular path to
        // be multikey, the plan will still be correct (if possibly sub-optimal). Conversely, if
        // we were to construct a query plan that incorrectly believes a path is NOT multikey,
        // it could produce incorrect results.
        OneOffRead oor(_opCtx, insertTime0.asTimestamp());
        MultikeyMetadataAccessStats stats;
        std::set<FieldRef> paths = getWildcardMultikeyPathSet(_opCtx, entry, &stats);
        ASSERT_EQUALS(1, paths.size());
        ASSERT_EQUALS("a", paths.begin()->dottedField());
    }
}

TEST_F(StorageTimestampTest, PrimarySetIndexMultikeyOnInsert) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.PrimarySetIndexMultikeyOnInsert");
    create(nss);

    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                 << static_cast<int>(kIndexVersion));
    ASSERT_OK(createIndexFromSpec(_opCtx, _clock, nss.ns_forTest(), indexSpec));

    const LogicalTime pastTime = _clock->tickClusterTime(1);
    const LogicalTime insertTime = pastTime.addTicks(1);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    BSONObj doc = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
    WriteUnitOfWork wunit(_opCtx);
    insertDocument(collAcq.getCollectionPtr(), InsertStatement(doc));
    wunit.commit();

    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, pastTime.asTimestamp(), false, {{}});
    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, insertTime.asTimestamp(), true, {{0}});
}

TEST_F(StorageTimestampTest, PrimarySetIndexMultikeyOnInsertUnreplicated) {
    // Use an unreplicated collection.
    repl::UnreplicatedWritesBlock noRep(_opCtx);
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.system.profile");
    create(nss);

    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "key" << BSON("a" << 1) << "v"
                                 << static_cast<int>(kIndexVersion));
    ASSERT_OK(createIndexFromSpec(_opCtx, _clock, nss.ns_forTest(), indexSpec));

    const LogicalTime pastTime = _clock->tickClusterTime(1);
    const LogicalTime insertTime = pastTime.addTicks(1);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    BSONObj doc = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));
    WriteUnitOfWork wunit(_opCtx);
    insertDocument(collAcq.getCollectionPtr(), InsertStatement(doc));
    wunit.commit();

    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, pastTime.asTimestamp(), true, {{0}});
    assertMultikeyPaths(
        _opCtx, collAcq.getCollectionPtr(), indexName, insertTime.asTimestamp(), true, {{0}});
}

TEST_F(StorageTimestampTest, PrimarySetsMultikeyInsideMultiDocumentTransaction) {
    auto service = _opCtx->getServiceContext();
    auto sessionCatalog = SessionCatalog::get(service);
    sessionCatalog->reset_forTest();
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx);
    mongoDSessionCatalog->onStepUp(_opCtx);

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.PrimarySetsMultikeyInsideMultiDocumentTransaction");
    create(nss);

    auto indexName = "a_1";
    auto indexSpec = BSON("name" << indexName << "ns" << nss.ns_forTest() << "key" << BSON("a" << 1)
                                 << "v" << static_cast<int>(kIndexVersion));
    auto doc = BSON("_id" << 1 << "a" << BSON_ARRAY(1 << 2));

    ASSERT_OK(createIndexFromSpec(_opCtx, _clock, nss.ns_forTest(), indexSpec));

    const auto currentTime = _clock->getTime();
    const auto presentTs = currentTime.clusterTime().asTimestamp();

    // This test does not run a real ReplicationCoordinator, so must advance the snapshot
    // manager manually.
    auto storageEngine = cc().getServiceContext()->getStorageEngine();
    storageEngine->getSnapshotManager()->setLastApplied(presentTs);

    const auto beforeTxnTime = _clock->tickClusterTime(1);
    auto beforeTxnTs = beforeTxnTime.asTimestamp();
    const auto multikeyNoopTime = beforeTxnTime.addTicks(1);
    auto multikeyNoopTs = multikeyNoopTime.asTimestamp();
    auto commitEntryTs = multikeyNoopTime.addTicks(1).asTimestamp();

    LOGV2(22502, "Present time", "timestamp"_attr = presentTs);
    LOGV2(22503, "Before transaction time", "timestamp"_attr = beforeTxnTs);
    LOGV2(4801000, "Multikey noop time", "timestamp"_attr = multikeyNoopTs);
    LOGV2(22504, "Commit entry time", "timestamp"_attr = commitEntryTs);

    const auto sessionId = makeLogicalSessionIdForTest();
    _opCtx->setLogicalSessionId(sessionId);
    _opCtx->setTxnNumber(1);
    _opCtx->setInMultiDocumentTransaction();

    // Check out the session.
    auto ocs = mongoDSessionCatalog->checkOutSession(_opCtx);

    auto txnParticipant = TransactionParticipant::get(_opCtx);
    ASSERT(txnParticipant);

    txnParticipant.beginOrContinue(_opCtx,
                                   {*_opCtx->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    txnParticipant.unstashTransactionResources(_opCtx, "insert");
    {
        // Insert a document that will set the index as multikey.
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        insertDocument(collAcq.getCollectionPtr(), InsertStatement(doc));
    }

    txnParticipant.commitUnpreparedTransaction(_opCtx);
    txnParticipant.stashTransactionResources(_opCtx);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    const auto& collPtr = collAcq.getCollectionPtr();

    // Make sure the transaction committed and its writes were timestamped correctly.
    assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
    assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
    assertDocumentAtTimestamp(collPtr, multikeyNoopTs, BSONObj());
    assertDocumentAtTimestamp(collPtr, commitEntryTs, doc);
    assertDocumentAtTimestamp(collPtr, _nullTs, doc);

    // Make sure the multikey write was timestamped correctly. For correctness, the timestamp of
    // the write that sets the multikey flag to true should be less than or equal to the first
    // write that made the index multikey, which, in this case, is the commit timestamp of the
    // transaction. In other words, it is not incorrect to assign a timestamp that is too early,
    // but it is incorrect to assign a timestamp that is too late. In this specific case, we
    // expect the write to be timestamped at the logical clock tick directly preceding the
    // commit time.
    assertMultikeyPaths(_opCtx, collPtr, indexName, presentTs, false /* shouldBeMultikey */, {{}});
    assertMultikeyPaths(
        _opCtx, collPtr, indexName, beforeTxnTs, false /* shouldBeMultikey */, {{}});
    assertMultikeyPaths(
        _opCtx, collPtr, indexName, multikeyNoopTs, true /* shouldBeMultikey */, {{0}});
    assertMultikeyPaths(
        _opCtx, collPtr, indexName, commitEntryTs, true /* shouldBeMultikey */, {{0}});
    assertMultikeyPaths(_opCtx, collPtr, indexName, _nullTs, true /* shouldBeMultikey */, {{0}});
}

TEST_F(StorageTimestampTest, InitializeMinValid) {
    NamespaceString nss = NamespaceString::kDefaultMinValidNamespace;
    create(nss);

    repl::ReplicationConsistencyMarkersImpl consistencyMarkers(repl::StorageInterface::get(_opCtx));
    consistencyMarkers.initializeMinValidDocument(_opCtx);

    repl::MinValidDocument expectedMinValid;
    expectedMinValid.setMinValidTerm(repl::OpTime::kUninitializedTerm);
    expectedMinValid.setMinValidTimestamp(_nullTs);

    assertMinValidDocumentAtTimestamp(nss, _nullTs, expectedMinValid);
    assertMinValidDocumentAtTimestamp(nss, _pastTs, expectedMinValid);
    assertMinValidDocumentAtTimestamp(nss, _presentTs, expectedMinValid);
    assertMinValidDocumentAtTimestamp(nss, _futureTs, expectedMinValid);
}

TEST_F(StorageTimestampTest, SetMinValidInitialSyncFlag) {
    NamespaceString nss = NamespaceString::kDefaultMinValidNamespace;
    create(nss);

    repl::ReplicationConsistencyMarkersImpl consistencyMarkers(repl::StorageInterface::get(_opCtx));
    ASSERT(consistencyMarkers.createInternalCollections(_opCtx).isOK());
    consistencyMarkers.initializeMinValidDocument(_opCtx);
    consistencyMarkers.setInitialSyncFlag(_opCtx);

    repl::MinValidDocument expectedMinValidWithSetFlag;
    expectedMinValidWithSetFlag.setMinValidTerm(repl::OpTime::kUninitializedTerm);
    expectedMinValidWithSetFlag.setMinValidTimestamp(_nullTs);
    expectedMinValidWithSetFlag.setInitialSyncFlag(true);

    assertMinValidDocumentAtTimestamp(nss, _nullTs, expectedMinValidWithSetFlag);

    consistencyMarkers.clearInitialSyncFlag(_opCtx);

    repl::MinValidDocument expectedMinValidWithUnsetFlag;
    expectedMinValidWithSetFlag.setMinValidTerm(repl::OpTime::kUninitializedTerm);
    expectedMinValidWithSetFlag.setMinValidTimestamp(_nullTs);

    assertMinValidDocumentAtTimestamp(nss, _nullTs, expectedMinValidWithUnsetFlag);
}

TEST_F(StorageTimestampTest, SetMinValidAppliedThrough) {
    NamespaceString nss = NamespaceString::kDefaultMinValidNamespace;
    create(nss);

    repl::ReplicationConsistencyMarkersImpl consistencyMarkers(repl::StorageInterface::get(_opCtx));
    consistencyMarkers.initializeMinValidDocument(_opCtx);

    repl::MinValidDocument expectedMinValidInit;
    expectedMinValidInit.setMinValidTerm(repl::OpTime::kUninitializedTerm);
    expectedMinValidInit.setMinValidTimestamp(_nullTs);
    assertMinValidDocumentAtTimestamp(nss, _nullTs, expectedMinValidInit);

    consistencyMarkers.setAppliedThrough(_opCtx, repl::OpTime(_presentTs, _presentTerm));

    repl::MinValidDocument expectedMinValidPresent;
    expectedMinValidPresent.setMinValidTerm(repl::OpTime::kUninitializedTerm);
    expectedMinValidPresent.setMinValidTimestamp(_nullTs);
    expectedMinValidPresent.setAppliedThrough(repl::OpTime(_presentTs, _presentTerm));

    assertMinValidDocumentAtTimestamp(nss, _nullTs, expectedMinValidPresent);

    // appliedThrough opTime can be unset.
    consistencyMarkers.clearAppliedThrough(_opCtx);
    assertMinValidDocumentAtTimestamp(nss, _nullTs, expectedMinValidInit);
}

/**
 * This KVDropDatabase test only exists in this file for historical reasons, the final phase of
 * timestamping `dropDatabase` side-effects no longer applies. The purpose of this test is to
 * exercise the `StorageEngine::dropDatabase` method.
 */
class KVDropDatabase : public StorageTimestampTest {
private:
    void _doTest() override {
        // Not actually called.
    }

public:
    void run(bool simulatePrimary) {
        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto mdbCatalog = storageEngine->getMDBCatalog();

        // Declare the database to be in a "synced" state, i.e: in steady-state replication.
        Timestamp syncTime = _clock->tickClusterTime(1).asTimestamp();
        invariant(!syncTime.isNull());
        storageEngine->setInitialDataTimestamp(syncTime);

        // This test drops collections piece-wise instead of having the "drop database" algorithm
        // perform this walk. Defensively operate on a separate DB from the other tests to ensure
        // no leftover collections carry-over.
        const NamespaceString nss =
            NamespaceString::createNamespaceString_forTest("unittestsDropDB.kvDropDatabase");
        const NamespaceString sysProfile =
            NamespaceString::createNamespaceString_forTest("unittestsDropDB.system.profile");

        std::string collIdent;
        std::string indexIdent;
        std::string sysProfileIdent;
        // `*.system.profile` does not have an `_id` index. Just create it to abide by the API. This
        // value will be the empty string. Helper methods accommodate this.
        std::string sysProfileIndexIdent;
        for (auto& tuple : {std::tie(nss, collIdent, indexIdent),
                            std::tie(sysProfile, sysProfileIdent, sysProfileIndexIdent)}) {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_X);
            ;

            // Save the pre-state idents so we can capture the specific idents related to collection
            // creation.
            std::vector<std::string> origIdents = mdbCatalog->getAllIdents(_opCtx);
            const auto& nss = std::get<0>(tuple);

            // Non-replicated namespaces are wrapped in an unreplicated writes block. This has the
            // side-effect of not timestamping the collection creation.
            repl::UnreplicatedWritesBlock notReplicated(_opCtx);
            if (nss.isReplicated()) {
                TimestampBlock tsBlock(_opCtx, _clock->tickClusterTime(1).asTimestamp());
                create(nss);
            } else {
                create(nss);
            }

            // Bind the local values to the variables in the parent scope.
            auto& collIdent = std::get<1>(tuple);
            auto& indexIdent = std::get<2>(tuple);
            std::tie(collIdent, indexIdent) = getNewCollectionIndexIdent(mdbCatalog, origIdents);
        }

        {
            // Drop/rename `kvDropDatabase`. `system.profile` does not get dropped/renamed.
            AutoGetCollection coll(_opCtx, nss, LockMode::MODE_X);
            WriteUnitOfWork wuow(_opCtx);
            Database* db = coll.getDb();
            ASSERT_OK(db->dropCollection(_opCtx, nss));
            wuow.commit();
        }

        // Reserve a tick, this represents a time after the rename in which the `kvDropDatabase`
        // ident for `kvDropDatabase` still exists.
        const Timestamp postRenameTime = _clock->tickClusterTime(1).asTimestamp();

        // Because the storage engine is managing drops internally, the ident should not be visible
        // after a drop.
        assertIdentsMissingAtTimestamp(mdbCatalog, collIdent, indexIdent, postRenameTime);

        const Timestamp dropTime = _clock->tickClusterTime(1).asTimestamp();
        if (simulatePrimary) {
            ASSERT_OK(dropDatabaseForApplyOps(_opCtx, nss.dbName()));
        } else {
            repl::UnreplicatedWritesBlock uwb(_opCtx);
            TimestampBlock ts(_opCtx, dropTime);
            ASSERT_OK(dropDatabaseForApplyOps(_opCtx, nss.dbName()));
        }

        // Assert that the idents do not exist.
        assertIdentsMissingAtTimestamp(
            mdbCatalog, sysProfileIdent, sysProfileIndexIdent, Timestamp::max());
        assertIdentsMissingAtTimestamp(mdbCatalog, collIdent, indexIdent, Timestamp::max());

        // dropDatabase must not timestamp the final write. The collection and index should seem
        // to have never existed.
        assertIdentsMissingAtTimestamp(mdbCatalog, collIdent, indexIdent, syncTime);

        // Reset initial data timestamp to avoid unintended storage engine timestamp side effects.
        storageEngine->setInitialDataTimestamp(Timestamp(0, 0));
    }
};

TEST(SimpleStorageTimestampTest, KVDropDatabasePrimary) {
    {
        KVDropDatabase test;
        test.run(true);
    }
    // Reconstruct the datafiles from scratch across tests.
    {
        KVDropDatabase test;
        test.run(false);
    }
}

/**
 * This test asserts that the catalog updates that represent the beginning and end of an index
 * build are timestamped. Additionally, the index will be `multikey` and that catalog update that
 * finishes the index build will also observe the index is multikey.
 *
 * Primaries log no-ops when starting an index build to acquire a timestamp. A primary committing
 * an index build gets timestamped when the `createIndexes` command creates an oplog entry. That
 * step is mimiced here.
 *
 * Secondaries timestamp starting their index build by being in a `TimestampBlock` when the oplog
 * entry is processed. Secondaries will look at the logical clock when completing the index
 * build. This is safe so long as completion is not racing with secondary oplog application.
 */
class TimestampIndexBuilds : public StorageTimestampTest {
private:
    void _doTest() override {
        // Not actually called.
    }

public:
    void run(bool simulatePrimary) {
        const bool simulateSecondary = !simulatePrimary;
        if (simulateSecondary) {
            // The MemberState is inspected during index builds to use a "ghost" write to timestamp
            // index completion.
            ASSERT_OK(_coordinatorMock->setFollowerMode({repl::MemberState::MS::RS_SECONDARY}));
        }

        auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        auto mdbCatalog = storageEngine->getMDBCatalog();

        NamespaceString nss =
            NamespaceString::createNamespaceString_forTest("unittests.timestampIndexBuilds");
        create(nss);

        AutoGetCollection autoColl(_opCtx, nss, LockMode::MODE_X);
        CollectionWriter coll(_opCtx, autoColl);

        RecordId catalogId = autoColl.getCollection()->getCatalogId();

        const LogicalTime insertTimestamp = _clock->tickClusterTime(1);
        {
            WriteUnitOfWork wuow(_opCtx);
            insertDocument(coll.get(),
                           InsertStatement(BSON("_id" << 0 << "a" << BSON_ARRAY(1 << 2)),
                                           insertTimestamp.asTimestamp(),
                                           _presentTerm));
            wuow.commit();
            ASSERT_EQ(1, itCount(autoColl.getCollection()));
        }

        const std::string indexIdent = "index-ident";

        // Build an index on `{a: 1}`. This index will be multikey.
        MultiIndexBlock indexer;
        ScopeGuard abortOnExit(
            [&] { indexer.abortIndexBuild(_opCtx, coll, MultiIndexBlock::kNoopOnCleanUpFn); });
        const LogicalTime beforeIndexBuild = _clock->tickClusterTime(2);
        BSONObj indexInfoObj;
        {
            // Primaries do not have a wrapping `TimestampBlock`; secondaries do.
            const Timestamp commitTimestamp =
                simulatePrimary ? Timestamp::min() : beforeIndexBuild.addTicks(1).asTimestamp();
            TimestampBlock tsBlock(_opCtx, commitTimestamp);

            // Secondaries will also be in an `UnreplicatedWritesBlock` that prevents the `logOp`
            // from making creating an entry.
            boost::optional<repl::UnreplicatedWritesBlock> unreplicated;
            if (simulateSecondary) {
                unreplicated.emplace(_opCtx);
            }

            auto indexBuildInfo = IndexBuildInfo(BSON("v" << 2 << "unique" << true << "name"
                                                          << "a_1"
                                                          << "key" << BSON("a" << 1)),
                                                 indexIdent);
            indexBuildInfo.setInternalIdents(*storageEngine, VersionContext::getDecoration(_opCtx));
            auto swIndexInfoObj = indexer.init(
                _opCtx,
                coll,
                {indexBuildInfo},
                MultiIndexBlock::makeTimestampedIndexOnInitFn(_opCtx, autoColl.getCollection()));
            ASSERT_OK(swIndexInfoObj.getStatus());
            indexInfoObj = std::move(swIndexInfoObj.getValue()[0]);
        }

        const LogicalTime afterIndexInit = _clock->tickClusterTime(2);

        // Inserting all the documents has the side-effect of setting internal state on the index
        // builder that the index is multikey.
        ASSERT_OK(indexer.insertAllDocumentsInCollection(_opCtx, nss));
        ASSERT_OK(indexer.checkConstraints(_opCtx, autoColl.getCollection()));

        {
            WriteUnitOfWork wuow(_opCtx);
            // All callers of `MultiIndexBlock::commit` are responsible for timestamping index
            // completion  Primaries write an oplog entry. Secondaries explicitly set a
            // timestamp.
            ASSERT_OK(indexer.commit(
                _opCtx,
                coll.getWritableCollection(_opCtx),
                [&](const BSONObj& indexSpec, StringData ident) {
                    if (simulatePrimary) {
                        // The timestamping responsibility for each index is placed
                        // on the caller.
                        auto indexBuildInfo = IndexBuildInfo(indexSpec, std::string{ident});
                        indexBuildInfo.setInternalIdents(*storageEngine,
                                                         VersionContext::getDecoration(_opCtx));
                        _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                            _opCtx, nss, coll->uuid(), indexBuildInfo, false);
                    } else {
                        const auto currentTime = _clock->getTime();
                        ASSERT_OK(shard_role_details::getRecoveryUnit(_opCtx)->setTimestamp(
                            currentTime.clusterTime().asTimestamp()));
                    }
                },
                MultiIndexBlock::kNoopOnCommitFn));
            wuow.commit();
        }
        abortOnExit.dismiss();

        const Timestamp afterIndexBuild = _clock->tickClusterTime(1).asTimestamp();

        assertIdentsMissingAtTimestamp(mdbCatalog, "", indexIdent, beforeIndexBuild.asTimestamp());

        // Assert that the index entry exists after init and `ready: false`.
        assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdent, afterIndexInit.asTimestamp());
        {
            ASSERT_FALSE(
                getIndexMetaData(
                    getMetaDataAtTime(mdbCatalog, catalogId, afterIndexInit.asTimestamp()), "a_1")
                    .ready);
        }

        // After the build completes, assert that the index is `ready: true` and multikey.
        assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdent, afterIndexBuild);
        {
            auto indexMetaData =
                getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, afterIndexBuild), "a_1");
            ASSERT(indexMetaData.ready);
            ASSERT(indexMetaData.multikey);

            ASSERT_EQ(std::size_t(1), indexMetaData.multikeyPaths.size());
            const bool match = indexMetaData.multikeyPaths[0] == MultikeyComponents({0});
            if (!match) {
                FAIL(std::string(str::stream() << "Expected: [ [ 0 ] ] Actual: "
                                               << dumpMultikeyPaths(indexMetaData.multikeyPaths)));
            }
        }
    }
};

TEST(SimpleStorageTimestampTest, TimestampIndexBuilds) {
    {
        TimestampIndexBuilds test;
        test.run(false);
    }
    // Reconstruct the datafiles from scratch across tests.
    {
        TimestampIndexBuilds test;
        test.run(true);
    }
}

TEST_F(StorageTimestampTest, TimestampMultiIndexBuilds) {
    const auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    const auto mdbCatalog = storageEngine->getMDBCatalog();

    // Create config.system.indexBuilds collection to store commit quorum value during index
    // building.
    ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(
        _opCtx, NamespaceString::kIndexBuildEntryNamespace));
    ASSERT_OK(
        createCollection(_opCtx,
                         NamespaceString::kIndexBuildEntryNamespace.dbName(),
                         BSON("create" << NamespaceString::kIndexBuildEntryNamespace.coll())));

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampMultiIndexBuilds");
    create(nss);

    std::vector<std::string> origIdents;
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        ;

        const LogicalTime insertTimestamp = _clock->tickClusterTime(1);

        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                       insertTimestamp.asTimestamp(),
                                       _presentTerm));
        wuow.commit();
        ASSERT_EQ(1, itCount(collAcq.getCollectionPtr()));

        // Save the pre-state idents so we can capture the specific ident related to index
        // creation.
        origIdents = mdbCatalog->getAllIdents(_opCtx);

        // Ensure we have a committed snapshot to avoid ReadConcernMajorityNotAvailableYet
        // error at the beginning of the the collection scan phase.
        auto snapshotManager = storageEngine->getSnapshotManager();
        snapshotManager->setCommittedSnapshot(insertTimestamp.asTimestamp());
    }

    DBDirectClient client(_opCtx);
    {
        // Disable index build commit quorum as we don't have support of replication subsystem
        // for voting.
        auto index1 = BSON("v" << kIndexVersion << "key" << BSON("a" << 1) << "name"
                               << "a_1");
        auto index2 = BSON("v" << kIndexVersion << "key" << BSON("b" << 1) << "name"
                               << "b_1");
        auto createIndexesCmdObj =
            BSON("createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(index1 << index2)
                                 << "commitQuorum" << 0);
        BSONObj result;
        ASSERT(client.runCommand(nss.dbName(), createIndexesCmdObj, result)) << result;
    }

    auto indexCreateInitTs =
        queryOplog(BSON("op" << "c"
                             << "o.startIndexBuild" << nss.coll() << "o.indexes.0.name"
                             << "a_1"))["ts"]
            .timestamp();
    auto commitIndexBuildTs =
        queryOplog(BSON("op" << "c"
                             << "o.commitIndexBuild" << nss.coll() << "o.indexes.0.name"
                             << "a_1"))["ts"]
            .timestamp();
    auto indexBComplete = commitIndexBuildTs;

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kRead),
        MODE_S);
    RecordId catalogId = collAcq.getCollectionPtr()->getCatalogId();

    // The idents are created and persisted with the "ready: false" write.
    // There should be two new index idents visible at this time.
    const std::vector<std::string> indexes =
        getNewIndexIdentsAtTime(mdbCatalog, origIdents, indexCreateInitTs);
    ASSERT_EQ(static_cast<std::size_t>(2), indexes.size()) << " Num idents: " << indexes.size();

    ASSERT_FALSE(
        getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, indexCreateInitTs), "a_1").ready);
    ASSERT_FALSE(
        getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, indexCreateInitTs), "b_1").ready);

    // Assert the `b_1` index becomes ready at the last oplog entry time.
    ASSERT_TRUE(
        getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, indexBComplete), "a_1").ready);
    ASSERT_TRUE(
        getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, indexBComplete), "b_1").ready);

    // Assert that the index build is removed from config.system.indexBuilds collection after
    // completion.
    {
        const auto collection =
            acquireCollForRead(_opCtx, NamespaceString::kIndexBuildEntryNamespace);
        ASSERT_TRUE(collection.exists());

        // At the commitIndexBuild entry time, the index build be still be present in the
        // indexBuilds collection.
        {
            OneOffRead oor(_opCtx, indexBComplete);
            // Fails if the collection is empty.
            findOne(collection.getCollectionPtr());
        }

        // After the index build has finished, we should not see the doc in the indexBuilds
        // collection.
        ASSERT_EQUALS(0, itCount(collection.getCollectionPtr()));
    }
}

TEST_F(StorageTimestampTest, TimestampMultiIndexBuildsDuringRename) {
    auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto mdbCatalog = storageEngine->getMDBCatalog();

    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "unittests.timestampMultiIndexBuildsDuringRename");
    create(nss);

    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        ;

        const LogicalTime insertTimestamp = _clock->tickClusterTime(1);

        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                       insertTimestamp.asTimestamp(),
                                       _presentTerm));
        wuow.commit();
        ASSERT_EQ(1, itCount(collAcq.getCollectionPtr()));

        // Ensure we have a committed snapshot to avoid ReadConcernMajorityNotAvailableYet
        // error at the beginning of the the collection scan phase.
        auto snapshotManager = storageEngine->getSnapshotManager();
        snapshotManager->setCommittedSnapshot(insertTimestamp.asTimestamp());
    }

    DBDirectClient client(_opCtx);
    {
        // Disable index build commit quorum as we don't have support of replication subsystem
        // for voting.
        auto index1 = BSON("v" << kIndexVersion << "key" << BSON("a" << 1) << "name"
                               << "a_1");
        auto index2 = BSON("v" << kIndexVersion << "key" << BSON("b" << 1) << "name"
                               << "b_1");
        auto createIndexesCmdObj =
            BSON("createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(index1 << index2)
                                 << "commitQuorum" << 0);
        BSONObj result;
        ASSERT(client.runCommand(nss.dbName(), createIndexesCmdObj, result)) << result;
    }

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
    ;

    NamespaceString renamedNss = NamespaceString::createNamespaceString_forTest(
        "unittestsRename.timestampMultiIndexBuildsDuringRename");
    create(renamedNss);

    // Save the pre-state idents so we can capture the specific ident related to index
    // creation.
    std::vector<std::string> origIdents = mdbCatalog->getAllIdents(_opCtx);

    // Rename collection.
    BSONObj renameResult;
    ASSERT(client.runCommand(DatabaseName::kAdmin,
                             BSON("renameCollection" << nss.ns_forTest() << "to"
                                                     << renamedNss.ns_forTest() << "dropTarget"
                                                     << true),
                             renameResult))
        << renameResult;

    NamespaceString tmpName;
    Timestamp indexCommitTs;
    // Empty temporary collections generate createIndexes oplog entry even if the node
    // supports 2 phase index build.
    const auto createIndexesDocument =
        queryOplog(BSON("ns" << renamedNss.db_forTest() + ".$cmd" << "o.createIndexes"
                             << BSON("$exists" << true) << "o.name" << "b_1"));
    const auto tmpCollName =
        createIndexesDocument.getObjectField("o").getStringField("createIndexes");
    tmpName = NamespaceString::createNamespaceString_forTest(renamedNss.db_forTest(), tmpCollName);
    indexCommitTs = createIndexesDocument["ts"].timestamp();
    const Timestamp indexCreateInitTs = queryOplog(BSON("op" << "c"
                                                             << "o.create" << tmpName.coll()))["ts"]
                                            .timestamp();


    // We expect one new collection ident and one new index ident (the _id index) during this
    // rename.
    assertRenamedCollectionIdentsAtTimestamp(
        mdbCatalog, origIdents, /*expectedNewIndexIdents*/ 1, indexCreateInitTs);

    // We expect one new collection ident and three new index idents (including the _id index)
    // after this rename. The a_1 and b_1 index idents are created and persisted with the
    // "ready: true" write.
    assertRenamedCollectionIdentsAtTimestamp(
        mdbCatalog, origIdents, /*expectedNewIndexIdents*/ 3, indexCommitTs);

    // Assert the 'a_1' and `b_1` indexes becomes ready at the last oplog entry time.
    RecordId renamedCatalogId = CollectionCatalog::get(_opCtx)
                                    ->lookupCollectionByNamespace(_opCtx, renamedNss)
                                    ->getCatalogId();
    ASSERT_TRUE(
        getIndexMetaData(getMetaDataAtTime(mdbCatalog, renamedCatalogId, indexCommitTs), "a_1")
            .ready);
    ASSERT_TRUE(
        getIndexMetaData(getMetaDataAtTime(mdbCatalog, renamedCatalogId, indexCommitTs), "b_1")
            .ready);
}

/**
 * This test asserts that the catalog updates that represent the beginning and end of an aborted
 * index build are timestamped. The oplog should contain two entries startIndexBuild and
 * abortIndexBuild. We will inspect the catalog at the timestamp corresponding to each of these
 * oplog entries.
 */
TEST_F(StorageTimestampTest, TimestampAbortIndexBuild) {
    auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto mdbCatalog = storageEngine->getMDBCatalog();

    // Create config.system.indexBuilds collection to store commit quorum value during index
    // building.
    ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(
        _opCtx, NamespaceString::kIndexBuildEntryNamespace));
    ASSERT_OK(
        createCollection(_opCtx,
                         NamespaceString::kIndexBuildEntryNamespace.dbName(),
                         BSON("create" << NamespaceString::kIndexBuildEntryNamespace.coll())));

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampAbortIndexBuild");
    create(nss);

    std::vector<std::string> origIdents;
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        ;

        auto insertTimestamp1 = _clock->tickClusterTime(1);
        auto insertTimestamp2 = _clock->tickClusterTime(1);

        // Insert two documents with the same value for field 'a' so that
        // we will fail to create a unique index.
        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(BSON("_id" << 0 << "a" << 1),
                                       insertTimestamp1.asTimestamp(),
                                       _presentTerm));
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(BSON("_id" << 1 << "a" << 1),
                                       insertTimestamp2.asTimestamp(),
                                       _presentTerm));
        wuow.commit();
        ASSERT_EQ(2, itCount(collAcq.getCollectionPtr()));

        // Save the pre-state idents so we can capture the specific ident related to index
        // creation.
        origIdents = mdbCatalog->getAllIdents(_opCtx);

        // Ensure we have a committed snapshot to avoid ReadConcernMajorityNotAvailableYet
        // error at the beginning of the the collection scan phase.
        auto snapshotManager = storageEngine->getSnapshotManager();
        snapshotManager->setCommittedSnapshot(insertTimestamp2.asTimestamp());
    }

    {
        // Disable index build commit quorum as we don't have support of replication subsystem
        // for voting.
        auto index1 = BSON("v" << kIndexVersion << "key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "unique" << true);
        auto createIndexesCmdObj =
            BSON("createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(index1) << "commitQuorum"
                                 << 0);

        DBDirectClient client(_opCtx);
        BSONObj result;
        ASSERT_FALSE(client.runCommand(nss.dbName(), createIndexesCmdObj, result));
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, getStatusFromCommandResult(result));
    }

    // Confirm that startIndexBuild and abortIndexBuild oplog entries have been written to the
    // oplog.
    auto indexStartDocument =
        queryOplog(BSON("ns" << nss.db_forTest() + ".$cmd" << "o.startIndexBuild" << nss.coll()
                             << "o.indexes.0.name"
                             << "a_1"));
    auto indexStartTs = indexStartDocument["ts"].timestamp();
    auto indexAbortDocument =
        queryOplog(BSON("ns" << nss.db_forTest() + ".$cmd" << "o.abortIndexBuild" << nss.coll()
                             << "o.indexes.0.name"
                             << "a_1"));
    auto indexAbortTs = indexAbortDocument["ts"].timestamp();

    // Check index state in catalog at oplog entry times for both startIndexBuild and
    // abortIndexBuild.
    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
    ;
    RecordId catalogId = collAcq.getCollectionPtr()->getCatalogId();

    // We expect one new one new index ident during this index build.
    assertRenamedCollectionIdentsAtTimestamp(
        mdbCatalog, origIdents, /*expectedNewIndexIdents*/ 1, indexStartTs);
    ASSERT_FALSE(
        getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, indexStartTs), "a_1").ready);

    // We expect all new idents to be removed after the index build has aborted.
    assertRenamedCollectionIdentsAtTimestamp(
        mdbCatalog, origIdents, /*expectedNewIndexIdents*/ 0, indexAbortTs);
    assertIndexMetaDataMissing(getMetaDataAtTime(mdbCatalog, catalogId, indexAbortTs), "a_1");

    // Assert that the index build is removed from config.system.indexBuilds collection after
    // completion.
    {
        const auto collection =
            acquireCollForRead(_opCtx, NamespaceString::kIndexBuildEntryNamespace);
        ASSERT_TRUE(collection.exists());

        // At the commitIndexBuild entry time, the index build be still be present in the
        // indexBuilds collection.
        {
            OneOffRead oor(_opCtx, indexAbortTs);
            // Fails if the collection is empty.
            findOne(collection.getCollectionPtr());
        }

        // After the index build has finished, we should not see the doc in the indexBuilds
        // collection.
        ASSERT_EQUALS(0, itCount(collection.getCollectionPtr()));
    }
}

TEST_F(StorageTimestampTest, TimestampIndexDropsWildcard) {
    auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto mdbCatalog = storageEngine->getMDBCatalog();

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampIndexDrops");
    create(nss);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
    ;
    CollectionWriter coll(_opCtx, &collAcq);

    const LogicalTime insertTimestamp = _clock->tickClusterTime(1);
    {
        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                       insertTimestamp.asTimestamp(),
                                       _presentTerm));
        wuow.commit();
        ASSERT_EQ(1, itCount(collAcq.getCollectionPtr()));
    }


    const Timestamp beforeIndexBuild = _clock->tickClusterTime(1).asTimestamp();

    // Save the pre-state idents so we can capture the specific ident related to index
    // creation.
    std::vector<std::string> origIdents = mdbCatalog->getAllIdents(_opCtx);

    std::vector<Timestamp> afterCreateTimestamps;
    std::vector<std::string> indexIdents;
    // Create an index and get the ident for each index.
    for (auto key : {"a", "b", "c"}) {
        createIndex(coll, str::stream() << key << "_1", BSON(key << 1));

        // Timestamps at the completion of each index build.
        afterCreateTimestamps.push_back(_clock->tickClusterTime(1).asTimestamp());

        // Add the new ident to the vector and reset the current idents.
        indexIdents.push_back(getNewIndexIdentAtTime(mdbCatalog, origIdents, Timestamp::min()));
        origIdents = mdbCatalog->getAllIdents(_opCtx);
    }

    // Ensure each index is visible at the correct timestamp, and not before.
    for (size_t i = 0; i < indexIdents.size(); i++) {
        auto beforeTs = (i == 0) ? beforeIndexBuild : afterCreateTimestamps[i - 1];
        assertIdentsMissingAtTimestamp(mdbCatalog, "", indexIdents[i], beforeTs);
        assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdents[i], afterCreateTimestamps[i]);
    }

    const auto currentTime = _clock->getTime();
    const LogicalTime beforeDropTs = currentTime.clusterTime();

    // Drop all of the indexes.
    dropIndexes(_opCtx, nss, boost::none, "*");

    // Assert that each index is dropped individually and with its own timestamp. The order of
    // dropping and creating are not guaranteed to be the same, but Assert all of the created
    // indexes were also dropped.
    size_t nIdents = indexIdents.size();
    for (size_t i = 0; i < nIdents; i++) {
        OneOffRead oor(_opCtx, beforeDropTs.addTicks(i + 1).asTimestamp());

        auto ident = getDroppedIndexIdent(mdbCatalog, origIdents);
        indexIdents.erase(std::remove(indexIdents.begin(), indexIdents.end(), ident),
                          indexIdents.end());

        origIdents = mdbCatalog->getAllIdents(_opCtx);
    }
    ASSERT_EQ(indexIdents.size(), 0ul) << "Dropped idents should match created idents";
}

TEST_F(StorageTimestampTest, TimestampIndexDropsListed) {
    auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto mdbCatalog = storageEngine->getMDBCatalog();

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampIndexDrops");
    create(nss);

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);
    ;
    CollectionWriter coll(_opCtx, &collAcq);

    const LogicalTime insertTimestamp = _clock->tickClusterTime(1);
    {
        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2 << "c" << 3),
                                       insertTimestamp.asTimestamp(),
                                       _presentTerm));
        wuow.commit();
        ASSERT_EQ(1, itCount(collAcq.getCollectionPtr()));
    }


    const Timestamp beforeIndexBuild = _clock->tickClusterTime(1).asTimestamp();

    // Save the pre-state idents so we can capture the specific ident related to index
    // creation.
    std::vector<std::string> origIdents = mdbCatalog->getAllIdents(_opCtx);

    std::vector<Timestamp> afterCreateTimestamps;
    std::vector<std::string> indexIdents;
    // Create an index and get the ident for each index.
    for (auto key : {"a", "b", "c"}) {
        createIndex(coll, str::stream() << key << "_1", BSON(key << 1));

        // Timestamps at the completion of each index build.
        afterCreateTimestamps.push_back(_clock->tickClusterTime(1).asTimestamp());

        // Add the new ident to the vector and reset the current idents.
        indexIdents.push_back(getNewIndexIdentAtTime(mdbCatalog, origIdents, Timestamp::min()));
        origIdents = mdbCatalog->getAllIdents(_opCtx);
    }

    // Ensure each index is visible at the correct timestamp, and not before.
    for (size_t i = 0; i < indexIdents.size(); i++) {
        auto beforeTs = (i == 0) ? beforeIndexBuild : afterCreateTimestamps[i - 1];
        assertIdentsMissingAtTimestamp(mdbCatalog, "", indexIdents[i], beforeTs);
        assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdents[i], afterCreateTimestamps[i]);
    }

    const auto currentTime = _clock->getTime();
    const LogicalTime beforeDropTs = currentTime.clusterTime();

    // Drop all of the indexes.
    dropIndexes(_opCtx, nss, boost::none, std::vector<std::string>{"a_1", "b_1", "c_1"});

    // Assert that each index is dropped individually and with its own timestamp. The order of
    // dropping and creating are not guaranteed to be the same, but Assert all of the created
    // indexes were also dropped.
    size_t nIdents = indexIdents.size();
    for (size_t i = 0; i < nIdents; i++) {
        OneOffRead oor(_opCtx, beforeDropTs.addTicks(i + 1).asTimestamp());

        auto ident = getDroppedIndexIdent(mdbCatalog, origIdents);
        indexIdents.erase(std::remove(indexIdents.begin(), indexIdents.end(), ident),
                          indexIdents.end());

        origIdents = mdbCatalog->getAllIdents(_opCtx);
    }
    ASSERT_EQ(indexIdents.size(), 0ul) << "Dropped idents should match created idents";
}

TEST_F(StorageTimestampTest, IndexBuildsResolveErrorsDuringStateChangeToPrimary) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampIndexBuilds");
    create(nss);

    const auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    auto collectionAcquisition = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);

    CollectionWriter collection(_opCtx, &collectionAcquisition);

    // Indexing of parallel arrays is not allowed, so these are deemed "bad".
    const auto badDoc1 = BSON("_id" << 0 << "a" << BSON_ARRAY(0 << 1) << "b" << BSON_ARRAY(0 << 1));
    const auto badDoc2 = BSON("_id" << 1 << "a" << BSON_ARRAY(2 << 3) << "b" << BSON_ARRAY(2 << 3));
    const auto badDoc3 = BSON("_id" << 2 << "a" << BSON_ARRAY(4 << 5) << "b" << BSON_ARRAY(4 << 5));

    // NOTE: This test does not test any timestamp reads.
    const LogicalTime insert1 = _clock->tickClusterTime(1);
    {
        LOGV2(22505, "inserting {badDoc1}", "badDoc1"_attr = badDoc1);
        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collection.get(),
                       InsertStatement(badDoc1, insert1.asTimestamp(), _presentTerm));
        wuow.commit();
    }

    const LogicalTime insert2 = _clock->tickClusterTime(1);
    {
        LOGV2(22506, "inserting {badDoc2}", "badDoc2"_attr = badDoc2);
        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collection.get(),
                       InsertStatement(badDoc2, insert2.asTimestamp(), _presentTerm));
        wuow.commit();
    }

    const IndexCatalogEntry* buildingIndex = nullptr;
    MultiIndexBlock indexer;
    ScopeGuard abortOnExit(
        [&] { indexer.abortIndexBuild(_opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn); });

    // Provide a build UUID, indicating that this is a two-phase index build.
    const auto buildUUID = UUID::gen();
    indexer.setTwoPhaseBuildUUID(buildUUID);

    const LogicalTime indexInit = _clock->tickClusterTime(3);

    // First, simulate being a secondary. Indexing errors are ignored.
    {
        ASSERT_OK(_coordinatorMock->setFollowerMode({repl::MemberState::MS::RS_SECONDARY}));
        _coordinatorMock->alwaysAllowWrites(false);
        repl::UnreplicatedWritesBlock unreplicatedWrites(_opCtx);

        {
            TimestampBlock tsBlock(_opCtx, indexInit.asTimestamp());

            auto indexBuildInfo = IndexBuildInfo(BSON("v" << 2 << "name"
                                                          << "a_1_b_1"
                                                          << "ns" << collection->ns().ns_forTest()
                                                          << "key" << BSON("a" << 1 << "b" << 1)),
                                                 std::string{"index-ident"});
            indexBuildInfo.setInternalIdents(*storageEngine, VersionContext::getDecoration(_opCtx));
            auto swSpecs = indexer.init(
                _opCtx,
                collection,
                {indexBuildInfo},
                MultiIndexBlock::makeTimestampedIndexOnInitFn(_opCtx, collection.get()));
            ASSERT_OK(swSpecs.getStatus());
        }

        auto indexCatalog = collection->getIndexCatalog();
        buildingIndex = indexCatalog->getEntry(indexCatalog->findIndexByName(
            _opCtx,
            "a_1_b_1",
            IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished));
        ASSERT(buildingIndex);

        ASSERT_OK(indexer.insertAllDocumentsInCollection(_opCtx, nss));

        ASSERT_TRUE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));

        // There should be one skipped record from the collection scan.
        ASSERT_FALSE(
            buildingIndex->indexBuildInterceptor()->getSkippedRecordTracker()->areAllRecordsApplied(
                _opCtx));
    }

    // As a primary, stop ignoring indexing errors.
    ASSERT_OK(_coordinatorMock->setFollowerMode({repl::MemberState::MS::RS_PRIMARY}));

    {
        // This write will not succeed because the node is a primary and the document is not
        // indexable.
        LOGV2(22507, "attempting to insert {badDoc3}", "badDoc3"_attr = badDoc3);
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_THROWS_CODE(
            collection_internal::insertDocument(
                _opCtx,
                collection.get(),
                InsertStatement(badDoc3, indexInit.addTicks(1).asTimestamp(), _presentTerm),
                /* opDebug */ nullptr,
                /* noWarn */ false),
            DBException,
            ErrorCodes::CannotIndexParallelArrays);
        wuow.commit();
    }

    // There should skipped records from failed collection scans and writes.
    ASSERT_FALSE(
        buildingIndex->indexBuildInterceptor()->getSkippedRecordTracker()->areAllRecordsApplied(
            _opCtx));
    // This fails because the bad record is still invalid.
    auto status = indexer.retrySkippedRecords(_opCtx, collection.get());
    ASSERT_EQ(status.code(), ErrorCodes::CannotIndexParallelArrays);

    ASSERT_FALSE(
        buildingIndex->indexBuildInterceptor()->getSkippedRecordTracker()->areAllRecordsApplied(
            _opCtx));
    ASSERT_TRUE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));

    // Update one documents to be valid, and delete the other. These modifications are written
    // to the side writes table and must be drained.
    Helpers::upsert(_opCtx, collectionAcquisition, BSON("_id" << 0 << "a" << 1 << "b" << 1));
    {
        RecordId badRecord = Helpers::findOne(_opCtx, collectionAcquisition, BSON("_id" << 1));
        WriteUnitOfWork wuow(_opCtx);
        collection_internal::deleteDocument(_opCtx,
                                            collectionAcquisition.getCollectionPtr(),
                                            kUninitializedStmtId,
                                            badRecord,
                                            nullptr);
        wuow.commit();
    }

    ASSERT_FALSE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));
    ASSERT_OK(indexer.drainBackgroundWrites(_opCtx,
                                            RecoveryUnit::ReadSource::kNoTimestamp,
                                            IndexBuildInterceptor::DrainYieldPolicy::kNoYield));


    // This succeeds because the bad documents are now either valid or removed.
    ASSERT_OK(indexer.retrySkippedRecords(_opCtx, collection.get()));
    ASSERT_TRUE(
        buildingIndex->indexBuildInterceptor()->getSkippedRecordTracker()->areAllRecordsApplied(
            _opCtx));
    ASSERT_TRUE(buildingIndex->indexBuildInterceptor()->areAllWritesApplied(_opCtx));
    ASSERT_OK(indexer.checkConstraints(_opCtx, collection.get()));

    {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(indexer.commit(
            _opCtx,
            collection.getWritableCollection(_opCtx),
            [&](const BSONObj& indexSpec, StringData ident) {
                auto indexBuildInfo = IndexBuildInfo(indexSpec, std::string{ident});
                indexBuildInfo.setInternalIdents(*storageEngine,
                                                 VersionContext::getDecoration(_opCtx));
                _opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                    _opCtx, collection->ns(), collection->uuid(), indexBuildInfo, false);
            },
            MultiIndexBlock::kNoopOnCommitFn));
        wuow.commit();
    }
    abortOnExit.dismiss();
}

/**
 * This test exercises the code path in which a primary performs an index build via oplog
 * application of a createIndexes oplog entry. In this code path, a primary timestamps the
 * index build through applying the oplog entry, rather than creating an oplog entry.
 */
TEST_F(StorageTimestampTest, TimestampIndexOplogApplicationOnPrimary) {
    RAIIServerParameterControllerForTest replicateLocalCatalogInfoController(
        "featureFlagReplicateLocalCatalogIdentifiers", true);

    // Index builds expect a non-empty oplog and a valid committed snapshot.
    {
        Lock::GlobalLock lk(_opCtx, MODE_IX);
        WriteUnitOfWork wuow(_opCtx);
        auto service = _opCtx->getServiceContext();
        service->getOpObserver()->onOpMessage(_opCtx, BSONObj());
        wuow.commit();

        auto snapshotManager = service->getStorageEngine()->getSnapshotManager();
        auto lastAppliedOpTime =
            repl::ReplicationCoordinator::get(service)->getMyLastAppliedOpTime();
        snapshotManager->setCommittedSnapshot(lastAppliedOpTime.getTimestamp());
    }

    // In order for oplog application to assign timestamps, we must be in non-replicated mode
    // and disable document validation.
    repl::UnreplicatedWritesBlock uwb(_opCtx);
    DisableDocumentValidation validationDisabler(_opCtx);

    std::string dbName = "unittest";
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(dbName, "oplogApplicationOnPrimary");
    BSONObj doc = BSON("_id" << 1 << "field" << 1);

    const LogicalTime setupStart = _clock->tickClusterTime(1);

    UUID collUUID = UUID::gen();
    {
        // Create the collection and insert a document.
        create(nss);
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        collUUID = collAcq.getCollectionPtr()->uuid();
        WriteUnitOfWork wuow(_opCtx);
        insertDocument(collAcq.getCollectionPtr(),
                       InsertStatement(doc, setupStart.asTimestamp(), _presentTerm));
        wuow.commit();
    }


    {
        // Sanity check everything exists.
        auto collAcq = acquireCollForRead(_opCtx, nss);
        ASSERT(collAcq.exists());

        const auto currentTime = _clock->getTime();
        const auto presentTs = currentTime.clusterTime().asTimestamp();
        assertDocumentAtTimestamp(collAcq.getCollectionPtr(), presentTs, doc);
    }

    // Simulate a scenario where the node is a primary, but does not accept writes. This is
    // the only scenario in which a primary can do an index build via oplog application, since
    // the applyOps command no longer allows createIndexes (see SERVER-41554).
    _coordinatorMock->alwaysAllowWrites(false);
    {
        const auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
        const auto beforeBuildTime = _clock->tickClusterTime(2);
        const auto startBuildTs = beforeBuildTime.addTicks(1).asTimestamp();

        auto keyPattern = BSON("field" << 1);
        auto spec = BSON("v" << 2 << "name" << "field_1" << "key" << keyPattern);
        auto startBuildOpTime = repl::OpTime(startBuildTs, _presentTerm);
        UUID indexBuildUUID = UUID::gen();
        const auto indexIdent = "index-ident"_sd;
        IndexBuildInfo indexBuildInfo(spec, std::string{indexIdent});
        indexBuildInfo.setInternalIdents(*storageEngine, VersionContext::getDecoration(_opCtx));

        // Wait for the index build thread to start the collection scan before proceeding with
        // checking the catalog and applying the commitIndexBuild oplog entry.
        // There is a potential race between applying the commitIndexBuild oplog entry and the
        // transitioning the index build thread's ReplIndexBuildState from kSetup to
        // kInProgress. This is due to the commit retry logic using the ClockSourceMock, rather
        // than an actual  system clock that advances automatically, through OperationContext's
        // waitFor() function.
        {
            FailPointEnableBlock fpb("hangAfterStartingIndexBuild");

            auto start = repl::makeStartIndexBuildOplogEntry(
                startBuildOpTime, nss, collUUID, indexBuildUUID, indexBuildInfo);
            const bool dataIsConsistent = true;
            ASSERT_OK(
                repl::applyOplogEntryOrGroupedInserts(_opCtx,
                                                      repl::ApplierOperation{&start},
                                                      repl::OplogApplication::Mode::kSecondary,
                                                      dataIsConsistent));

            // We cannot use the OperationContext to wait for the thread to reach the fail point
            // because it also uses the ClockSourceMock.
            fpb->waitForTimesEntered(Interruptible::notInterruptible(),
                                     fpb.initialTimesEntered() + 1);
        }

        auto mdbCatalog = _opCtx->getServiceContext()->getStorageEngine()->getMDBCatalog();

        {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kRead),
                                             MODE_IS);
            assertIdentsMissingAtTimestamp(
                mdbCatalog, "", indexIdent, beforeBuildTime.asTimestamp());
            assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdent, startBuildTs);

            // The index has not committed yet, so it is not ready.
            RecordId catalogId = collAcq.getCollectionPtr()->getCatalogId();
            ASSERT_FALSE(
                getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, startBuildTs), "field_1")
                    .ready);
        }  // release read lock so commit index build oplog entry can take its own locks.

        auto commit = repl::makeCommitIndexBuildOplogEntry(
            startBuildOpTime, nss, collUUID, indexBuildUUID, indexBuildInfo);
        const bool dataIsConsistent = true;
        ASSERT_OK(repl::applyOplogEntryOrGroupedInserts(_opCtx,
                                                        repl::ApplierOperation{&commit},
                                                        repl::OplogApplication::Mode::kSecondary,
                                                        dataIsConsistent));

        // Reacquire read lock to check index metadata.
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
        RecordId catalogId = collAcq.getCollectionPtr()->getCatalogId();
        ASSERT_TRUE(
            getIndexMetaData(getMetaDataAtTime(mdbCatalog, catalogId, startBuildTs), "field_1")
                .ready);
    }
}

TEST_F(StorageTimestampTest, ViewCreationSeparateTransaction) {
    const NamespaceString backingCollNss =
        NamespaceString::createNamespaceString_forTest("unittests.backingColl");
    create(backingCollNss);

    const NamespaceString viewNss =
        NamespaceString::createNamespaceString_forTest("unittests.view");
    const NamespaceString systemViewsNss = NamespaceString::makeSystemDotViewsNamespace(
        DatabaseName::createDatabaseName_forTest(boost::none, "unittests"));

    ASSERT_OK(createCollection(_opCtx,
                               viewNss.dbName(),
                               BSON("create" << viewNss.coll() << "pipeline" << BSONArray()
                                             << "viewOn" << backingCollNss.coll())));

    const Timestamp systemViewsCreateTs =
        queryOplog(BSON("op" << "c"
                             << "ns" << (viewNss.db_forTest() + ".$cmd") << "o.create"
                             << "system.views"))["ts"]
            .timestamp();
    const Timestamp viewCreateTs = queryOplog(BSON("op" << "i"
                                                        << "ns" << systemViewsNss.ns_forTest()
                                                        << "o._id" << viewNss.ns_forTest()))["ts"]
                                       .timestamp();

    const auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    const auto mdbCatalog = storageEngine->getMDBCatalog();
    {
        auto collAcq =
            acquireCollection(_opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  _opCtx, systemViewsNss, AcquisitionPrerequisites::kRead),
                              MODE_IS);
        RecordId catalogId = collAcq.getCollectionPtr()->getCatalogId();

        auto systemViewsMd =
            getMetaDataAtTime(mdbCatalog, catalogId, Timestamp(systemViewsCreateTs.asULL() - 1));
        ASSERT(systemViewsMd == nullptr)
            << systemViewsNss.toStringForErrorMsg()
            << " incorrectly exists before creation. CreateTs: " << systemViewsCreateTs;

        systemViewsMd = getMetaDataAtTime(mdbCatalog, catalogId, systemViewsCreateTs);
        auto nss = systemViewsMd->nss;
        ASSERT_EQ(systemViewsNss, nss);

        assertDocumentAtTimestamp(collAcq.getCollectionPtr(), systemViewsCreateTs, BSONObj());
        assertDocumentAtTimestamp(collAcq.getCollectionPtr(),
                                  viewCreateTs,
                                  BSON("_id" << viewNss.ns_forTest() << "viewOn"
                                             << backingCollNss.coll() << "pipeline"
                                             << BSONArray()));
    }
}

TEST_F(StorageTimestampTest, CreateCollectionWithSystemIndex) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.system.users");

    ASSERT_FALSE(acquireCollForRead(_opCtx, nss).exists());

    ASSERT_OK(createCollection(_opCtx, nss.dbName(), BSON("create" << nss.coll())));

    RecordId catalogId;
    {
        auto collAcq = acquireCollForRead(_opCtx, nss);
        ASSERT(collAcq.getCollectionPtr());
        catalogId = collAcq.getCollectionPtr()->getCatalogId();
    }

    BSONObj result = queryOplog(BSON("op" << "c"
                                          << "ns" << nss.getCommandNS().ns_forTest() << "o.create"
                                          << nss.coll()));
    repl::OplogEntry op(result);
    // The logOp() call for createCollection should have timestamp 'futureTs', which will also
    // be the timestamp at which we do the write which creates the collection. Thus we expect
    // the collection to appear at 'futureTs' and not before.
    ASSERT_EQ(op.getTimestamp(), _futureTs) << op.toBSONForLogging();

    // The index build emits three oplog entries.
    Timestamp indexStartTs;
    Timestamp indexCreateTs;
    Timestamp indexCompleteTs;
    // Empty collections generate createIndexes oplog entry even if the node
    // supports 2 phase index build.
    indexStartTs = op.getTimestamp();
    indexCreateTs =
        repl::OplogEntry(
            queryOplog(BSON(
                "op" << "c"
                     << "ns" << nss.getCommandNS().ns_forTest() << "o.createIndexes" << nss.coll()
                     << (shouldReplicateLocalCatalogIdentifers(
                             rss::ReplicatedStorageService::get(_opCtx).getPersistenceProvider(),
                             VersionContext::getDecoration(_opCtx))
                             ? "o.spec.name"
                             : "o.name")
                     << "user_1_db_1")))
            .getTimestamp();
    indexCompleteTs = indexCreateTs;

    assertNamespaceInIdents(nss, _pastTs, false);
    assertNamespaceInIdents(nss, _presentTs, false);
    assertNamespaceInIdents(nss, _futureTs, true);
    assertNamespaceInIdents(nss, indexStartTs, true);
    if (!indexCreateTs.isNull()) {
        assertNamespaceInIdents(nss, indexCreateTs, true);
    }
    assertNamespaceInIdents(nss, indexCompleteTs, true);
    assertNamespaceInIdents(nss, _nullTs, true);

    ASSERT_GT(indexCompleteTs, _futureTs);
    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kRead),
        MODE_IS);
    const auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    const auto mdbCatalog = storageEngine->getMDBCatalog();
    const auto indexIdent = mdbCatalog->getIndexIdent(_opCtx, catalogId, "user_1_db_1");
    assertIdentsMissingAtTimestamp(mdbCatalog, "", indexIdent, _pastTs);
    assertIdentsMissingAtTimestamp(mdbCatalog, "", indexIdent, _presentTs);
    assertIdentsMissingAtTimestamp(mdbCatalog, "", indexIdent, _futureTs);
    // This is the timestamp of the startIndexBuild oplog entry, which is timestamped before the
    // index is created as part of the createIndexes oplog entry.
    assertIdentsMissingAtTimestamp(mdbCatalog, "", indexIdent, indexStartTs);
    if (!indexCreateTs.isNull()) {
        assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdent, indexCreateTs);
    }
    assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdent, indexCompleteTs);
    assertIdentsExistAtTimestamp(mdbCatalog, "", indexIdent, _nullTs);
}

TEST_F(StorageTimestampTest, MultipleTimestampsForMultikeyWrites) {
    RecordId catalogId;

    // Create config.system.indexBuilds collection to store commit quorum value during index
    // building.
    ASSERT_OK(repl::StorageInterface::get(_opCtx)->dropCollection(
        _opCtx, NamespaceString::kIndexBuildEntryNamespace));
    ASSERT_OK(
        createCollection(_opCtx,
                         NamespaceString::kIndexBuildEntryNamespace.dbName(),
                         BSON("create" << NamespaceString::kIndexBuildEntryNamespace.coll())));

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("unittests.timestampVectoredInsertMultikey");
    create(nss);

    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        WriteUnitOfWork wuow(_opCtx);
        insertDocument(
            collAcq.getCollectionPtr(),
            InsertStatement(BSON("_id" << 0 << "a" << 1 << "b" << 2), Timestamp(), _presentTerm));
        wuow.commit();
        ASSERT_EQ(1, itCount(collAcq.getCollectionPtr()));
        catalogId = collAcq.getCollectionPtr()->getCatalogId();
    }

    DBDirectClient client(_opCtx);
    {
        auto index1 = BSON("v" << kIndexVersion << "key" << BSON("a" << 1) << "name"
                               << "a_1");
        auto index2 = BSON("v" << kIndexVersion << "key" << BSON("b" << 1) << "name"
                               << "b_1");
        // Disable index build commit quorum as we don't have support of replication subsystem for
        // voting.
        auto createIndexesCmdObj =
            BSON("createIndexes" << nss.coll() << "indexes" << BSON_ARRAY(index1 << index2)
                                 << "commitQuorum" << 0);
        BSONObj result;
        ASSERT(client.runCommand(nss.dbName(), createIndexesCmdObj, result)) << result;
    }

    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    Timestamp tsBeforeMultikeyWrites = getTopOfOplog();
    {
        const LogicalTime firstInsertTime = _clock->tickClusterTime(2);

        std::vector<InsertStatement> vectoredInsert;
        vectoredInsert.emplace_back(BSON("_id" << 1 << "b" << BSON_ARRAY(1 << 2)),
                                    firstInsertTime.asTimestamp(),
                                    _presentTerm);
        vectoredInsert.emplace_back(BSON("_id" << 2 << "a" << BSON_ARRAY(1 << 2)),
                                    firstInsertTime.addTicks(1).asTimestamp(),
                                    _presentTerm);

        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(collection_internal::insertDocuments(_opCtx,
                                                       collAcq.getCollectionPtr(),
                                                       vectoredInsert.begin(),
                                                       vectoredInsert.end(),
                                                       nullptr,
                                                       false));
        wuow.commit();
    }

    // By virtue of not crashing due to WT observing an out of order write, this test has
    // succeeded. For completeness, check the index metadata.
    const auto storageEngine = _opCtx->getServiceContext()->getStorageEngine();
    const auto mdbCatalog = storageEngine->getMDBCatalog();
    std::shared_ptr<durable_catalog::CatalogEntryMetaData> mdBeforeInserts =
        getMetaDataAtTime(mdbCatalog, catalogId, tsBeforeMultikeyWrites);
    ASSERT_FALSE(getIndexMetaData(mdBeforeInserts, "a_1").multikey);
    ASSERT_FALSE(getIndexMetaData(mdBeforeInserts, "b_1").multikey);

    std::shared_ptr<durable_catalog::CatalogEntryMetaData> mdAfterInserts =
        getMetaDataAtTime(mdbCatalog, catalogId, getTopOfOplog());
    ASSERT(getIndexMetaData(mdAfterInserts, "a_1").multikey);
    ASSERT(getIndexMetaData(mdAfterInserts, "b_1").multikey);
}

class RetryableFindAndModifyTest : public StorageTimestampTest {
public:
    const StringData dbName = "unittest"_sd;
    const BSONObj oldObj = BSON("_id" << 0 << "a" << 1);

    RetryableFindAndModifyTest()
        : nss(NamespaceString::createNamespaceString_forTest(dbName,
                                                             "retryableFindAndModifyTest")) {
        auto service = _opCtx->getServiceContext();
        auto sessionCatalog = SessionCatalog::get(service);
        sessionCatalog->reset_forTest();
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx);
        mongoDSessionCatalog->onStepUp(_opCtx);

        create(nss);
        UUID ui = UUID::gen();

        {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_IX);
            ASSERT(collAcq.exists());
            ui = collAcq.uuid();
        }

        const auto currentTime = _clock->getTime();
        currentTs = currentTime.clusterTime().asTimestamp();
        insertTs = currentTime.clusterTime().asTimestamp() + 1;
        beforeOplogTs = insertTs + 1;
        oplogTs = insertTs + 2;
        // This test does not run a real ReplicationCoordinator, so must advance the snapshot
        // manager manually.
        auto storageEngine = cc().getServiceContext()->getStorageEngine();
        storageEngine->getSnapshotManager()->setLastApplied(insertTs);

        const auto sessionId = makeLogicalSessionIdForTest();
        _opCtx->setLogicalSessionId(sessionId);
        const auto txnNumber = 10;
        _opCtx->setTxnNumber(txnNumber);

        ocs = mongoDSessionCatalog->checkOutSession(_opCtx);

        {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_IX);
            WriteUnitOfWork wunit(_opCtx);
            insertDocument(collAcq.getCollectionPtr(), InsertStatement(oldObj));
            wunit.commit();
        }
        assertOplogDocumentExistsAtTimestamp(BSON("ts" << insertTs << "op"
                                                       << "i"),
                                             insertTs,
                                             true);

        storageEngine->getSnapshotManager()->setLastApplied(insertTs);

        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);
        // Start a retryable write.
        txnParticipant.beginOrContinue(_opCtx,
                                       {txnNumber},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);
    }

protected:
    NamespaceString nss;
    Timestamp currentTs;
    Timestamp insertTs;
    Timestamp beforeOplogTs;
    Timestamp oplogTs;

    std::unique_ptr<MongoDSessionCatalog::Session> ocs;
};

TEST_F(RetryableFindAndModifyTest, RetryableFindAndModifyUpdate) {
    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);

    CollectionWriter collection(_opCtx, &collAcq);
    const auto criteria = BSON("_id" << 0);
    const auto newObj = BSON("_id" << 0 << "a" << 1 << "b" << 1);
    CollectionUpdateArgs args{oldObj};
    args.criteria = criteria;
    args.stmtIds = {1};
    args.updatedDoc = newObj;
    args.storeDocOption = CollectionUpdateArgs::StoreDocOption::PreImage;
    args.update = BSON("$set" << BSON("b" << 1));
    args.retryableWrite = true;

    {
        auto cursor = collection->getCursor(_opCtx);
        auto record = cursor->next();
        invariant(record);
        WriteUnitOfWork wuow(_opCtx);
        collection_internal::updateDocument(
            _opCtx,
            collection.get(),
            record->id,
            Snapshotted<BSONObj>(shard_role_details::getRecoveryUnit(_opCtx)->getSnapshotId(),
                                 oldObj),
            newObj,
            collection_internal::kUpdateNoIndexes,
            nullptr /* indexesAffected */,
            nullptr /* opDebug */,
            &args);
        wuow.commit();
    }

    // There should be no oplog entry at 'beforeOplogTs'.
    const auto beforeOplogTsFilter = BSON("ts" << beforeOplogTs);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, currentTs, false);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, beforeOplogTs, false);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, oplogTs, false);

    const auto oplogTsFilter = BSON("ts" << oplogTs << "op"
                                         << "u");
    assertOplogDocumentExistsAtTimestamp(oplogTsFilter, currentTs, false);
    assertOplogDocumentExistsAtTimestamp(oplogTsFilter, beforeOplogTs, false);
    assertOplogDocumentExistsAtTimestamp(oplogTsFilter, oplogTs, true);
}

TEST_F(RetryableFindAndModifyTest, RetryableFindAndModifyUpdateWithDamages) {
    namespace mmb = mongo::mutablebson;
    const auto bsonObj = BSON("_id" << 0 << "a" << 1);
    // Create a new document representing BSONObj with the above contents.
    mmb::Document doc(bsonObj, mmb::Document::kInPlaceEnabled);

    DamageVector damages;
    const char* source = nullptr;
    size_t size = 0;
    ASSERT_TRUE(doc.getInPlaceUpdates(&damages, &source, &size));

    // Enable in-place mutation for this document
    ASSERT_EQUALS(mmb::Document::kInPlaceEnabled, doc.getCurrentInPlaceMode());
    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);

    CollectionWriter collection(_opCtx, &collAcq);
    const auto criteria = BSON("_id" << 0);
    const auto newObj = BSON("_id" << 0 << "a" << 0);
    CollectionUpdateArgs args{oldObj};
    args.criteria = criteria;
    args.stmtIds = {1};
    args.updatedDoc = newObj;
    args.storeDocOption = CollectionUpdateArgs::StoreDocOption::PreImage;
    args.update = BSON("$set" << BSON("a" << 0));
    args.retryableWrite = true;

    {
        Snapshotted<BSONObj> objSnapshot(
            shard_role_details::getRecoveryUnit(_opCtx)->getSnapshotId(), oldObj);
        auto cursor = collection->getCursor(_opCtx);
        auto record = cursor->next();
        invariant(record);
        WriteUnitOfWork wuow(_opCtx);
        const auto statusWith =
            collection_internal::updateDocumentWithDamages(_opCtx,
                                                           collAcq.getCollectionPtr(),
                                                           record->id,
                                                           objSnapshot,
                                                           source,
                                                           damages,
                                                           collection_internal::kUpdateNoIndexes,
                                                           nullptr /* indexesAffected */,
                                                           nullptr /* opDebug */,
                                                           &args);
        wuow.commit();
        ASSERT_OK(statusWith.getStatus());
    }

    // There should be no oplog entry at 'beforeOplogTs'.
    const auto beforeOplogTsFilter = BSON("ts" << beforeOplogTs);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, currentTs, false);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, beforeOplogTs, false);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, oplogTs, false);

    const auto tsFilter = BSON("ts" << oplogTs << "op"
                                    << "u");
    assertOplogDocumentExistsAtTimestamp(tsFilter, currentTs, false);
    assertOplogDocumentExistsAtTimestamp(tsFilter, beforeOplogTs, false);
    assertOplogDocumentExistsAtTimestamp(tsFilter, oplogTs, true);
}

TEST_F(RetryableFindAndModifyTest, RetryableFindAndModifyDelete) {
    auto collAcq = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_X);

    CollectionWriter collection(_opCtx, &collAcq);
    const auto bsonObj = BSON("_id" << 0 << "a" << 1);

    {
        Snapshotted<BSONObj> objSnapshot(
            shard_role_details::getRecoveryUnit(_opCtx)->getSnapshotId(), oldObj);
        auto cursor = collection->getCursor(_opCtx);
        auto record = cursor->next();
        invariant(record);
        WriteUnitOfWork wuow(_opCtx);
        collection_internal::deleteDocument(_opCtx,
                                            collAcq.getCollectionPtr(),
                                            objSnapshot,
                                            1,
                                            record->id,
                                            nullptr,
                                            false,
                                            false,
                                            collection_internal::StoreDeletedDoc::On,
                                            CheckRecordId::Off,
                                            collection_internal::RetryableWrite::kYes);
        wuow.commit();
    }

    // There should be no oplog entry at 'beforeOplogTs'.
    const auto beforeOplogTsFilter = BSON("ts" << beforeOplogTs);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, currentTs, false);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, beforeOplogTs, false);
    assertOplogDocumentExistsAtTimestamp(beforeOplogTsFilter, oplogTs, false);

    const auto tsFilter = BSON("ts" << oplogTs << "op"
                                    << "d");
    assertOplogDocumentExistsAtTimestamp(tsFilter, currentTs, false);
    assertOplogDocumentExistsAtTimestamp(tsFilter, beforeOplogTs, false);
    assertOplogDocumentExistsAtTimestamp(tsFilter, oplogTs, true);
}

class MultiDocumentTransactionTest : public StorageTimestampTest {
public:
    const StringData dbName = "unittest"_sd;
    const BSONObj doc = BSON("_id" << 1 << "TestValue" << 1);
    const BSONObj docKey = BSON("_id" << 1);

    MultiDocumentTransactionTest()
        : nss(NamespaceString::createNamespaceString_forTest(dbName,
                                                             "multiDocumentTransactionTest")) {
        auto service = _opCtx->getServiceContext();
        auto sessionCatalog = SessionCatalog::get(service);
        sessionCatalog->reset_forTest();
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(_opCtx);
        mongoDSessionCatalog->onStepUp(_opCtx);

        create(nss);
        UUID ui = UUID::gen();
        {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_IX);
            ASSERT(collAcq.exists());
            ui = collAcq.uuid();
        }

        const auto currentTime = _clock->getTime();
        presentTs = currentTime.clusterTime().asTimestamp();
        // This test does not run a real ReplicationCoordinator, so must advance the snapshot
        // manager manually.
        auto storageEngine = cc().getServiceContext()->getStorageEngine();
        storageEngine->getSnapshotManager()->setLastApplied(presentTs);
        const auto beforeTxnTime = _clock->tickClusterTime(1);
        beforeTxnTs = beforeTxnTime.asTimestamp();
        commitEntryTs = beforeTxnTime.addTicks(1).asTimestamp();

        const auto sessionId = makeLogicalSessionIdForTest();
        _opCtx->setLogicalSessionId(sessionId);
        _opCtx->setTxnNumber(26);
        _opCtx->setInMultiDocumentTransaction();

        ocs = mongoDSessionCatalog->checkOutSession(_opCtx);

        auto txnParticipant = TransactionParticipant::get(_opCtx);
        ASSERT(txnParticipant);

        txnParticipant.beginOrContinue(_opCtx,
                                       {*_opCtx->getTxnNumber()},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        txnParticipant.unstashTransactionResources(_opCtx, "insert");
        {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_IX);
            insertDocument(collAcq.getCollectionPtr(), InsertStatement(doc));
        }
        txnParticipant.stashTransactionResources(_opCtx);

        {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_IX);
            const auto& collPtr = collAcq.getCollectionPtr();
            assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
            assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
            assertDocumentAtTimestamp(collPtr, commitEntryTs, BSONObj());
            assertDocumentAtTimestamp(collPtr, _nullTs, BSONObj());

            const auto commitFilter = BSON("ts" << commitEntryTs);
            assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
            assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, false);
        }
    }

    void logTimestamps() const {
        LOGV2(22508, "Present TS: {presentTs}", "presentTs"_attr = presentTs);
        LOGV2(22509, "Before transaction TS: {beforeTxnTs}", "beforeTxnTs"_attr = beforeTxnTs);
        LOGV2(22510, "Commit entry TS: {commitEntryTs}", "commitEntryTs"_attr = commitEntryTs);
    }

    BSONObj getSessionTxnInfoAtTimestamp(const Timestamp& ts, bool expected) {
        auto collAcq = acquireCollection(_opCtx,
                                         CollectionAcquisitionRequest::fromOpCtx(
                                             _opCtx,
                                             NamespaceString::kSessionTransactionsTableNamespace,
                                             AcquisitionPrerequisites::kWrite),
                                         LockMode::MODE_IX);
        const auto sessionId = *_opCtx->getLogicalSessionId();
        const auto txnNum = *_opCtx->getTxnNumber();
        BSONObj doc;
        OneOffRead oor(_opCtx, ts);
        bool found = Helpers::findOne(
            _opCtx, collAcq, BSON("_id" << sessionId.toBSON() << "txnNum" << txnNum), doc);
        if (expected) {
            ASSERT(found) << "Missing session transaction info at " << ts;
        } else {
            ASSERT_FALSE(found) << "Session transaction info at " << ts
                                << " is unexpectedly present " << doc;
        }
        return doc;
    }

protected:
    NamespaceString nss;
    Timestamp presentTs;
    Timestamp beforeTxnTs;
    Timestamp commitEntryTs;

    std::unique_ptr<MongoDSessionCatalog::Session> ocs;
};

TEST_F(MultiDocumentTransactionTest, MultiDocumentTransaction) {
    auto txnParticipant = TransactionParticipant::get(_opCtx);
    ASSERT(txnParticipant);
    logTimestamps();

    txnParticipant.unstashTransactionResources(_opCtx, "insert");

    txnParticipant.commitUnpreparedTransaction(_opCtx);

    txnParticipant.stashTransactionResources(_opCtx);
    assertNoStartOpTime();
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, commitEntryTs, doc);
        assertDocumentAtTimestamp(collPtr, _nullTs, doc);

        const auto commitFilter = BSON("ts" << commitEntryTs);
        assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, true);
    }
}

// Including this class in a test fixture forces transactions to use one oplog entry per operation
// instead of packing them into as few oplog entries as fit.  This allows testing of the timestamps
// of multi-oplog-entry transactions.
class MultiOplogScopedSettings {
public:
    MultiOplogScopedSettings()
        : _prevPackingLimit(gMaxNumberOfTransactionOperationsInSingleOplogEntry) {
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = 1;
    }
    ~MultiOplogScopedSettings() {
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = _prevPackingLimit;
    }

private:
    int _prevPackingLimit;
};

TEST_F(MultiDocumentTransactionTest, MultiOplogEntryTransaction) {
    MultiOplogScopedSettings multiOplogSettings;

    const auto currentTime = _clock->getTime();
    const auto clusterTime = currentTime.clusterTime();
    Timestamp firstOplogEntryTs = clusterTime.addTicks(1).asTimestamp();
    commitEntryTs = clusterTime.addTicks(2).asTimestamp();

    auto txnParticipant = TransactionParticipant::get(_opCtx);
    ASSERT(txnParticipant);
    logTimestamps();

    txnParticipant.unstashTransactionResources(_opCtx, "insert");

    const BSONObj doc2 = BSON("_id" << 2 << "TestValue" << 2);
    const BSONObj doc2Key = BSON("_id" << 2);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        insertDocument(collAcq.getCollectionPtr(), InsertStatement(doc2));
    }
    txnParticipant.commitUnpreparedTransaction(_opCtx);

    txnParticipant.stashTransactionResources(_opCtx);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            LockMode::MODE_IX);
        const BSONObj query1 = BSON("_id" << 1);
        const BSONObj query2 = BSON("_id" << 2);
        const auto& collPtr = collAcq.getCollectionPtr();

        // Collection should be empty until commit, at which point both documents
        // should show up.
        assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, firstOplogEntryTs, BSONObj());
        assertFilteredDocumentAtTimestamp(collAcq, query1, commitEntryTs, doc);
        assertFilteredDocumentAtTimestamp(collAcq, query2, commitEntryTs, doc2);
        assertFilteredDocumentAtTimestamp(collAcq, query1, _nullTs, doc);
        assertFilteredDocumentAtTimestamp(collAcq, query2, _nullTs, doc2);

        // Implicit commit oplog entry should exist at commitEntryTs.
        const auto commitFilter =
            BSON("ts" << commitEntryTs << "o"
                      << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                                 << "ns" << nss.ns_forTest() << "ui"
                                                                 << collAcq.uuid() << "o" << doc2
                                                                 << "o2" << doc2Key))
                                         << "count" << 2));
        assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, firstOplogEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, true);

        // Check that the oldestActiveTxnTimestamp properly accounts for in-progress
        // transactions.
        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, firstOplogEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, _nullTs);

        // first oplog entry should exist at firstOplogEntryTs and after it.
        const auto firstOplogEntryFilter = BSON(
            "ts" << firstOplogEntryTs << "o"
                 << BSON("applyOps"
                         << BSON_ARRAY(BSON("op" << "i"
                                                 << "ns" << nss.ns_forTest() << "ui"
                                                 << collAcq.uuid() << "o" << doc << "o2" << docKey))
                         << "partialTxn" << true));
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, firstOplogEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, _nullTs, true);

        // Session state should go to inProgress at firstOplogEntryTs, then to committed
        // at commitEntryTs
        getSessionTxnInfoAtTimestamp(presentTs, false);
        getSessionTxnInfoAtTimestamp(beforeTxnTs, false);
        auto sessionInfo = getSessionTxnInfoAtTimestamp(firstOplogEntryTs, true);
        ASSERT_EQ(sessionInfo["state"].String(), "inProgress");
        ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), firstOplogEntryTs);
        ASSERT_EQ(sessionInfo["startOpTime"]["ts"].timestamp(), firstOplogEntryTs);

        sessionInfo = getSessionTxnInfoAtTimestamp(commitEntryTs, true);
        ASSERT_EQ(sessionInfo["state"].String(), "committed");
        ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), commitEntryTs);
        ASSERT_FALSE(sessionInfo.hasField("startOpTime"));

        sessionInfo = getSessionTxnInfoAtTimestamp(_nullTs, true);
        ASSERT_EQ(sessionInfo["state"].String(), "committed");
        ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), commitEntryTs);
        ASSERT_FALSE(sessionInfo.hasField("startOpTime"));
    }
}

TEST_F(MultiDocumentTransactionTest, CommitPreparedMultiOplogEntryTransaction) {
    MultiOplogScopedSettings multiOplogSettings;

    const auto currentTime = _clock->getTime();
    const auto clusterTime = currentTime.clusterTime();
    Timestamp firstOplogEntryTs = clusterTime.addTicks(1).asTimestamp();
    Timestamp prepareEntryTs = clusterTime.addTicks(2).asTimestamp();
    Timestamp commitTs = clusterTime.addTicks(3).asTimestamp();
    Timestamp commitEntryTs = clusterTime.addTicks(4).asTimestamp();

    auto txnParticipant = TransactionParticipant::get(_opCtx);
    ASSERT(txnParticipant);
    LOGV2(22511, "PrepareTS: {prepareEntryTs}", "prepareEntryTs"_attr = prepareEntryTs);
    logTimestamps();

    const auto prepareFilter = BSON("ts" << prepareEntryTs);
    const auto commitFilter = BSON("ts" << commitEntryTs);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, firstOplogEntryTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, prepareEntryTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, commitEntryTs, BSONObj());

        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, firstOplogEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, false);

        assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, prepareEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, false);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(boost::none, prepareEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, _nullTs);
    }
    txnParticipant.unstashTransactionResources(_opCtx, "insert");
    const BSONObj doc2 = BSON("_id" << 2 << "TestValue" << 2);
    const BSONObj doc2Key = BSON("_id" << 2);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        insertDocument(collAcq.getCollectionPtr(), InsertStatement(doc2));
    }
    txnParticipant.prepareTransaction(_opCtx, {});

    const BSONObj query1 = BSON("_id" << 1);
    const BSONObj query2 = BSON("_id" << 2);

    txnParticipant.stashTransactionResources(_opCtx);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, firstOplogEntryTs, BSONObj());

        {
            IgnorePrepareBlock ignorePrepare(_opCtx);
            // Perform the following while ignoring prepare conflicts. These calls would
            // otherwise wait forever until the prepared transaction committed or aborted.
            assertDocumentAtTimestamp(collPtr, prepareEntryTs, BSONObj());
            assertDocumentAtTimestamp(collPtr, commitEntryTs, BSONObj());
            assertDocumentAtTimestamp(collPtr, _nullTs, BSONObj());
        }

        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, firstOplogEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        // We haven't committed the prepared transaction
        assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, firstOplogEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, prepareEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, false);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, firstOplogEntryTs);
        assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, prepareEntryTs);
        // The transaction has not been committed yet and is still considered active.
        assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, commitEntryTs);
        assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, _nullTs);
    }

    txnParticipant.unstashTransactionResources(_opCtx, "commitTransaction");

    {
        FailPointEnableBlock failPointBlock("skipCommitTxnCheckPrepareMajorityCommitted");
        txnParticipant.commitPreparedTransaction(_opCtx, commitTs, {});
    }

    txnParticipant.stashTransactionResources(_opCtx);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, firstOplogEntryTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, prepareEntryTs, BSONObj());
        assertFilteredDocumentAtTimestamp(collAcq, query1, commitEntryTs, doc);
        assertFilteredDocumentAtTimestamp(collAcq, query2, commitEntryTs, doc2);
        assertFilteredDocumentAtTimestamp(collAcq, query1, _nullTs, doc);
        assertFilteredDocumentAtTimestamp(collAcq, query2, _nullTs, doc2);

        // The prepare oplog entry should exist at prepareEntryTs and onwards.
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        // The commit oplog entry should exist at commitEntryTs and onwards.
        assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, prepareEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, true);

        // The first oplog entry should exist at firstOplogEntryTs and onwards.
        const auto firstOplogEntryFilter = BSON(
            "ts" << firstOplogEntryTs << "o"
                 << BSON("applyOps"
                         << BSON_ARRAY(BSON("op" << "i"
                                                 << "ns" << nss.ns_forTest() << "ui"
                                                 << collAcq.uuid() << "o" << doc << "o2" << docKey))
                         << "partialTxn" << true));
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, firstOplogEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, prepareEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(firstOplogEntryFilter, _nullTs, true);
        // The prepare oplog entry should exist at prepareEntryTs and onwards.
        const auto prepareOplogEntryFilter =
            BSON("ts" << prepareEntryTs << "o"
                      << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                                 << "ns" << nss.ns_forTest() << "ui"
                                                                 << collAcq.uuid() << "o" << doc2
                                                                 << "o2" << doc2Key))
                                         << "prepare" << true << "count" << 2));
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, firstOplogEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, prepareEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, _nullTs, true);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, firstOplogEntryTs);
        assertOldestActiveTxnTimestampEquals(firstOplogEntryTs, prepareEntryTs);
        // The transaction is no longer considered active after being committed.
        assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, _nullTs);

        // The session state should go to inProgress at firstOplogEntryTs, then to prepared at
        // prepareEntryTs, and then finally to committed at commitEntryTs.
        auto sessionInfo = getSessionTxnInfoAtTimestamp(firstOplogEntryTs, true);
        ASSERT_EQ(sessionInfo["state"].String(), "inProgress");
        ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), firstOplogEntryTs);
        ASSERT_EQ(sessionInfo["startOpTime"]["ts"].timestamp(), firstOplogEntryTs);

        sessionInfo = getSessionTxnInfoAtTimestamp(prepareEntryTs, true);
        ASSERT_EQ(sessionInfo["state"].String(), "prepared");
        ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), prepareEntryTs);
        ASSERT_EQ(sessionInfo["startOpTime"]["ts"].timestamp(), firstOplogEntryTs);

        sessionInfo = getSessionTxnInfoAtTimestamp(_nullTs, true);
        ASSERT_EQ(sessionInfo["state"].String(), "committed");
        ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), commitEntryTs);
        ASSERT_FALSE(sessionInfo.hasField("startOpTime"));
    }
}

TEST_F(MultiDocumentTransactionTest, AbortPreparedMultiOplogEntryTransaction) {
    Timestamp firstOplogEntryTs, prepareEntryTs, abortEntryTs;
    MultiOplogScopedSettings multiOplogSettings;

    const auto currentTime = _clock->getTime();
    const auto clusterTime = currentTime.clusterTime();
    prepareEntryTs = clusterTime.addTicks(1).asTimestamp();
    abortEntryTs = clusterTime.addTicks(2).asTimestamp();

    auto txnParticipant = TransactionParticipant::get(_opCtx);
    ASSERT(txnParticipant);
    LOGV2(22512, "PrepareTS: {prepareEntryTs}", "prepareEntryTs"_attr = prepareEntryTs);
    LOGV2(22513, "AbortTS: {abortEntryTs}", "abortEntryTs"_attr = abortEntryTs);

    const auto prepareFilter = BSON("ts" << prepareEntryTs);
    const auto abortFilter = BSON("ts" << abortEntryTs);
    {
        assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, prepareEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, _nullTs, false);
    }
    txnParticipant.unstashTransactionResources(_opCtx, "insert");

    txnParticipant.prepareTransaction(_opCtx, {});

    txnParticipant.stashTransactionResources(_opCtx);
    {
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, firstOplogEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, prepareEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, _nullTs, false);
    }

    txnParticipant.unstashTransactionResources(_opCtx, "abortTransaction");

    txnParticipant.abortTransaction(_opCtx);

    txnParticipant.stashTransactionResources(_opCtx);
    {
        // The prepare oplog entry should exist at prepareEntryTs and onwards.
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        // The abort oplog entry should exist at abortEntryTs and onwards.
        assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, prepareEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(abortFilter, _nullTs, true);

        UUID ui = UUID::gen();
        {
            auto collAcq = acquireCollection(_opCtx,
                                             CollectionAcquisitionRequest::fromOpCtx(
                                                 _opCtx, nss, AcquisitionPrerequisites::kWrite),
                                             MODE_IX);
            ASSERT(collAcq.exists());
            ui = collAcq.uuid();
        }

        // The prepare oplog entry should exist at firstOplogEntryTs and onwards.
        const auto prepareOplogEntryFilter = BSON(
            "ts" << prepareEntryTs << "o"
                 << BSON("applyOps" << BSON_ARRAY(BSON("op" << "i"
                                                            << "ns" << nss.ns_forTest() << "ui"
                                                            << ui << "o" << doc << "o2" << docKey))
                                    << "prepare" << true));
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, prepareEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, abortEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareOplogEntryFilter, _nullTs, true);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(boost::none, abortEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, _nullTs);

        // The session state should be "aborted" at abortEntryTs.
        auto sessionInfo = getSessionTxnInfoAtTimestamp(abortEntryTs, true);
        ASSERT_EQ(sessionInfo["state"].String(), "aborted");
        ASSERT_EQ(sessionInfo["lastWriteOpTime"]["ts"].timestamp(), abortEntryTs);
        ASSERT_FALSE(sessionInfo.hasField("startOpTime"));
    }
}

TEST_F(MultiDocumentTransactionTest, PreparedMultiDocumentTransaction) {
    auto txnParticipant = TransactionParticipant::get(_opCtx);
    ASSERT(txnParticipant);

    const auto currentTime = _clock->getTime();
    const auto clusterTime = currentTime.clusterTime();
    const auto prepareTs = clusterTime.addTicks(1).asTimestamp();
    const auto commitTs = clusterTime.addTicks(2).asTimestamp();
    commitEntryTs = clusterTime.addTicks(3).asTimestamp();
    LOGV2(22514, "Prepare TS: {prepareTs}", "prepareTs"_attr = prepareTs);
    logTimestamps();

    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, prepareTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, commitEntryTs, BSONObj());

        const auto prepareFilter = BSON("ts" << prepareTs);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, false);

        const auto commitFilter = BSON("ts" << commitEntryTs);
        assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
    }
    txnParticipant.unstashTransactionResources(_opCtx, "insert");

    txnParticipant.prepareTransaction(_opCtx, {});

    txnParticipant.stashTransactionResources(_opCtx);
    assertHasStartOpTime();
    {
        const auto prepareFilter = BSON("ts" << prepareTs);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        const auto commitFilter = BSON("ts" << commitEntryTs);
        assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, false);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, _nullTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, commitEntryTs);
    }
    txnParticipant.unstashTransactionResources(_opCtx, "commitTransaction");

    {
        FailPointEnableBlock failPointBlock("skipCommitTxnCheckPrepareMajorityCommitted");
        txnParticipant.commitPreparedTransaction(_opCtx, commitTs, {});
    }

    assertNoStartOpTime();

    txnParticipant.stashTransactionResources(_opCtx);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, prepareTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, commitEntryTs, doc);
        assertDocumentAtTimestamp(collPtr, _nullTs, doc);

        const auto prepareFilter = BSON("ts" << prepareTs);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        const auto commitFilter = BSON("ts" << commitEntryTs);
        assertOplogDocumentExistsAtTimestamp(commitFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, commitEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(commitFilter, _nullTs, true);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
        assertOldestActiveTxnTimestampEquals(boost::none, commitEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, _nullTs);
    }
}

TEST_F(MultiDocumentTransactionTest, AbortedPreparedMultiDocumentTransaction) {
    auto txnParticipant = TransactionParticipant::get(_opCtx);
    ASSERT(txnParticipant);

    const auto currentTime = _clock->getTime();
    const auto clusterTime = currentTime.clusterTime();
    const auto prepareTs = clusterTime.addTicks(1).asTimestamp();
    const auto abortEntryTs = clusterTime.addTicks(2).asTimestamp();
    LOGV2(22515, "Prepare TS: {prepareTs}", "prepareTs"_attr = prepareTs);
    logTimestamps();

    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, prepareTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, abortEntryTs, BSONObj());

        const auto prepareFilter = BSON("ts" << prepareTs);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, false);

        const auto commitFilter = BSON("ts" << abortEntryTs);
        assertOplogDocumentExistsAtTimestamp(commitFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(commitFilter, abortEntryTs, false);
    }
    txnParticipant.unstashTransactionResources(_opCtx, "insert");

    txnParticipant.prepareTransaction(_opCtx, {});

    txnParticipant.stashTransactionResources(_opCtx);
    assertHasStartOpTime();
    {
        const auto prepareFilter = BSON("ts" << prepareTs);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        const auto abortFilter = BSON("ts" << abortEntryTs);
        assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, _nullTs, false);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, _nullTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, abortEntryTs);
    }
    txnParticipant.unstashTransactionResources(_opCtx, "abortTransaction");

    txnParticipant.abortTransaction(_opCtx);
    assertNoStartOpTime();

    txnParticipant.stashTransactionResources(_opCtx);
    {
        auto collAcq = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        const auto& collPtr = collAcq.getCollectionPtr();
        assertDocumentAtTimestamp(collPtr, presentTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, beforeTxnTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, prepareTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, abortEntryTs, BSONObj());
        assertDocumentAtTimestamp(collPtr, _nullTs, BSONObj());

        const auto prepareFilter = BSON("ts" << prepareTs);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, prepareTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, abortEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(prepareFilter, _nullTs, true);

        const auto abortFilter = BSON("ts" << abortEntryTs);
        assertOplogDocumentExistsAtTimestamp(abortFilter, presentTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, beforeTxnTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, prepareTs, false);
        assertOplogDocumentExistsAtTimestamp(abortFilter, abortEntryTs, true);
        assertOplogDocumentExistsAtTimestamp(abortFilter, _nullTs, true);

        assertOldestActiveTxnTimestampEquals(boost::none, presentTs);
        assertOldestActiveTxnTimestampEquals(boost::none, beforeTxnTs);
        assertOldestActiveTxnTimestampEquals(prepareTs, prepareTs);
        assertOldestActiveTxnTimestampEquals(boost::none, abortEntryTs);
        assertOldestActiveTxnTimestampEquals(boost::none, _nullTs);
    }
}
}  // namespace mongo
