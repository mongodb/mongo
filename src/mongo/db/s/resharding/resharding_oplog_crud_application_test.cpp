/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ReshardingOplogCrudApplicationTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto serviceContext = getServiceContext();

        const HostAndPort kConfigHostAndPort{"DummyConfig", 12345};
        ShardingState::get(serviceContext)
            ->setRecoveryCompleted({OID::gen(),
                                    ClusterRole::ShardServer,
                                    ConnectionString(kConfigHostAndPort),
                                    _myDonorId});
        {
            auto opCtx = makeOperationContext();
            auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
            ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
            repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::createOplog(opCtx.get());

            auto storageImpl = std::make_unique<repl::StorageInterfaceImpl>();
            repl::StorageInterface::set(serviceContext, std::move(storageImpl));

            MongoDSessionCatalog::set(
                serviceContext,
                std::make_unique<MongoDSessionCatalog>(
                    std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx.get());
            mongoDSessionCatalog->onStepUp(opCtx.get());
            LogicalSessionCache::set(serviceContext, std::make_unique<LogicalSessionCacheNoop>());

            // OpObserverImpl is required for timestamping the writes from
            // ReshardingOplogApplicationRules.
            auto opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            invariant(opObserverRegistry);

            opObserverRegistry->addObserver(
                std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
            opObserverRegistry->addObserver(
                std::make_unique<MigrationChunkClonerSourceOpObserver>());
        }

        {
            auto opCtx = makeOperationContext();

            for (const auto& nss : {_outputNss, _myStashNss, _otherStashNss}) {
                resharding::data_copy::ensureCollectionExists(
                    opCtx.get(), nss, CollectionOptions{});
            }

            {
                auto coll = acquireCollection(
                    opCtx.get(),
                    CollectionAcquisitionRequest{_outputNss,
                                                 PlacementConcern::kPretendUnsharded,
                                                 repl::ReadConcernArgs::get(opCtx.get()),
                                                 AcquisitionPrerequisites::kWrite},
                    MODE_X);
                CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx.get(),
                                                                                     _outputNss)
                    ->setFilteringMetadata(
                        opCtx.get(),
                        CollectionMetadata(makeChunkManagerForOutputCollection(), _myDonorId));
            }

            _metrics =
                ReshardingMetrics::makeInstance_forTest(_sourceUUID,
                                                        BSON(_newShardKey << 1),
                                                        _outputNss,
                                                        ReshardingMetricsCommon::Role::kRecipient,
                                                        serviceContext->getFastClockSource()->now(),
                                                        serviceContext);
            _oplogApplierMetrics = std::make_unique<ReshardingOplogApplierMetrics>(
                _myDonorId, _metrics.get(), boost::none);
            _applier = std::make_unique<ReshardingOplogApplicationRules>(
                _outputNss,
                std::vector<NamespaceString>{_myStashNss, _otherStashNss},
                0U,
                _myDonorId,
                makeChunkManagerForSourceCollection(),
                _oplogApplierMetrics.get());
        }
    }

    ReshardingOplogApplicationRules* applier() {
        return _applier.get();
    }

    StringData sk() {
        return _currentShardKey;
    }

    const NamespaceString& outputNss() {
        return _outputNss;
    }

    const NamespaceString& myStashNss() {
        return _myStashNss;
    }

    const NamespaceString& otherStashNss() {
        return _otherStashNss;
    }

    repl::OplogEntry makeInsertOp(BSONObj document) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kInsert);
        op.setNss(_sourceNss);
        op.setObject(std::move(document));

        // These are unused by ReshardingOplogApplicationRules but required by IDL parsing.
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    repl::OplogEntry makeUpdateOp(BSONObj filter, BSONObj update) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kUpdate);
        op.setNss(_sourceNss);
        op.setObject(std::move(update));
        op.setObject2(std::move(filter));

        // These are unused by ReshardingOplogApplicationRules but required by IDL parsing.
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    repl::OplogEntry makeDeleteOp(BSONObj document) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kDelete);
        op.setNss(_sourceNss);
        op.setObject(std::move(document));

        // These are unused by ReshardingOplogApplicationRules but required by IDL parsing.
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    void checkCollectionContents(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const std::vector<BSONObj>& documents) {
        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS);
        ASSERT_TRUE(coll.exists())
            << "Collection '" << nss.toStringForErrorMsg() << "' does not exist";

        auto exec = InternalPlanner::indexScan(
            opCtx,
            coll,
            coll.getCollectionPtr()->getIndexCatalog()->findIdIndex(opCtx),
            BSONObj(),
            BSONObj(),
            BoundInclusion::kIncludeStartKeyOnly,
            PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
            InternalPlanner::FORWARD,
            InternalPlanner::IXSCAN_FETCH);

        size_t i = 0;
        BSONObj obj;
        while (exec->getNext(&obj, nullptr) == PlanExecutor::ADVANCED) {
            ASSERT_LT(i, documents.size())
                << "Found extra document in collection: " << nss.toStringForErrorMsg() << ": "
                << obj;
            ASSERT_BSONOBJ_BINARY_EQ(obj, documents[i]);
            ++i;
        }

        if (i < documents.size()) {
            FAIL("Didn't find document in collection: ")
                << nss.toStringForErrorMsg() << ": " << documents[i];
        }
    }

    struct SlimApplyOpsInfo {
        BSONObj rawCommand;
        std::vector<repl::DurableReplOperation> operations;
    };

    std::vector<SlimApplyOpsInfo> findApplyOpsNewerThan(OperationContext* opCtx, Timestamp ts) {
        std::vector<SlimApplyOpsInfo> result;

        PersistentTaskStore<repl::OplogEntryBase> store(NamespaceString::kRsOplogNamespace);
        store.forEach(opCtx,
                      BSON("op" << "c"
                                << "o.applyOps" << BSON("$exists" << true) << "ts"
                                << BSON("$gt" << ts)),
                      [&](const auto& oplogEntry) {
                          auto applyOpsCmd = oplogEntry.getObject().getOwned();
                          auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(applyOpsCmd);

                          std::vector<repl::DurableReplOperation> operations;
                          operations.reserve(applyOpsInfo.getOperations().size());

                          for (const auto& innerOp : applyOpsInfo.getOperations()) {
                              operations.emplace_back(repl::DurableReplOperation::parse(
                                  innerOp, IDLParserContext{"findApplyOpsNewerThan"}));
                          }

                          result.emplace_back(
                              SlimApplyOpsInfo{std::move(applyOpsCmd), std::move(operations)});
                          return true;
                      });

        return result;
    }

private:
    ChunkManager makeChunkManager(const OID& epoch,
                                  const NamespaceString& nss,
                                  const UUID& uuid,
                                  const BSONObj& shardKey,
                                  const std::vector<ChunkType>& chunks) {
        auto rt = RoutingTableHistory::makeNew(nss,
                                               uuid,
                                               shardKey,
                                               false, /* unsplittable */
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);
        return ChunkManager(makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    ChunkManager makeChunkManagerForSourceCollection() {
        // Create three chunks, two that are owned by this donor shard and one owned by some other
        // shard. The chunk for {sk: null} is owned by this donor shard to allow test cases to omit
        // the shard key field when it isn't relevant.
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {
            ChunkType{
                _sourceUUID,
                ChunkRange{BSON(_currentShardKey << MINKEY),
                           BSON(_currentShardKey << -std::numeric_limits<double>::infinity())},
                ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
                _myDonorId},
            ChunkType{_sourceUUID,
                      ChunkRange{BSON(_currentShardKey << -std::numeric_limits<double>::infinity()),
                                 BSON(_currentShardKey << 0)},
                      ChunkVersion({epoch, Timestamp(1, 1)}, {100, 1}),
                      _otherDonorId},
            ChunkType{_sourceUUID,
                      ChunkRange{BSON(_currentShardKey << 0), BSON(_currentShardKey << MAXKEY)},
                      ChunkVersion({epoch, Timestamp(1, 1)}, {100, 2}),
                      _myDonorId}};

        return makeChunkManager(
            epoch, _sourceNss, _sourceUUID, BSON(_currentShardKey << 1), chunks);
    }

    ChunkManager makeChunkManagerForOutputCollection() {
        const OID epoch = OID::gen();
        const UUID outputUuid = UUID::gen();
        std::vector<ChunkType> chunks = {
            ChunkType{outputUuid,
                      ChunkRange{BSON(_newShardKey << MINKEY), BSON(_newShardKey << MAXKEY)},
                      ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
                      _myDonorId}};

        return makeChunkManager(epoch, _outputNss, outputUuid, BSON(_newShardKey << 1), chunks);
    }

    RoutingTableHistoryValueHandle makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "sk";
    const StringData _newShardKey = "new_sk";

    const NamespaceString _sourceNss =
        NamespaceString::createNamespaceString_forTest("test_crud", "collection_being_resharded");
    const UUID _sourceUUID = UUID::gen();

    const ShardId _myDonorId{"myDonorId"};
    const ShardId _otherDonorId{"otherDonorId"};

    const NamespaceString _outputNss =
        resharding::constructTemporaryReshardingNss(_sourceNss, _sourceUUID);
    const NamespaceString _myStashNss =
        resharding::getLocalConflictStashNamespace(_sourceUUID, _myDonorId);
    const NamespaceString _otherStashNss =
        resharding::getLocalConflictStashNamespace(_sourceUUID, _otherDonorId);

    service_context_test::ShardRoleOverride _shardRole;

    std::unique_ptr<ReshardingOplogApplicationRules> _applier;
    std::unique_ptr<ReshardingMetrics> _metrics;
    std::unique_ptr<ReshardingOplogApplierMetrics> _oplogApplierMetrics;
};

TEST_F(ReshardingOplogCrudApplicationTest, InsertOpInsertsIntoOuputCollection) {
    // This case tests applying rule #2 described in
    // ReshardingOplogApplicationRules::_applyInsert_inlock.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 1))));
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 2))));
    }

    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 1), BSON("_id" << 2)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, InsertOpBecomesReplacementUpdateOnOutputCollection) {
    // This case tests applying rule #3 described in
    // ReshardingOplogApplicationRules::_applyInsert_inlock.
    //
    // Make sure a document with {_id: 0} exists in the output collection before applying an insert
    // with the same _id. This donor shard owns these documents under the original shard key (it
    // owns the range {sk: 0} -> {sk: maxKey}).
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << 1))));

        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << 1)});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << 2))));
    }

    // We should have replaced the existing document in the output collection.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << 2)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, InsertOpWritesToStashCollectionAfterConflict) {
    // This case tests applying rules #1 and #4 described in
    // ReshardingOplogApplicationRules::_applyInsert_inlock.
    //
    // Make sure a document with {_id: 0} exists in the output collection before applying inserts
    // with the same _id. This donor shard does not own the document {_id: 0, sk: -1} under the
    // original shard key, so we should apply rule #4 and insert the document into the stash
    // collection.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << -1))));

        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << 2))));
    }

    // The output collection should still hold the document {_id: 0, sk: -1}, and the document with
    // {_id: 0, sk: 2} should have been inserted into the stash collection.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {BSON("_id" << 0 << sk() << 2)});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << 3))));
    }

    // The output collection should still hold the document {_id: 0, sk: 1}. We should have applied
    // rule #1 and turned the last insert op into a replacement update on the stash collection, so
    // the document {_id: 0, sk: 3} should now exist in the stash collection.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {BSON("_id" << 0 << sk() << 3)});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, UpdateOpModifiesStashCollectionAfterInsertConflict) {
    // This case tests applying rule #1 described in
    // ReshardingOplogApplicationRules::_applyUpdate_inlock.
    //
    // Insert a document {_id: 0} in the output collection before applying the insert of document
    // with {_id: 0}. This will force the document {_id: 0, sk: 2} to be inserted to the stash
    // collection for this donor shard.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << -1))));
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << 2))));

        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {BSON("_id" << 0 << sk() << 2)});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(
            opCtx.get(),
            makeUpdateOp(BSON("_id" << 0),
                         update_oplog_entry::makeDeltaOplogEntry(
                             BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 1))))));
    }

    // We should have applied rule #1 and updated the document with {_id: 0} in the stash collection
    // for this donor to now have the new field 'x'. And the output collection should remain
    // unchanged.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(
            opCtx.get(), myStashNss(), {BSON("_id" << 0 << sk() << 2 << "x" << 1)});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, UpdateOpIsNoopWhenDifferentOwningDonorOrNotMatching) {
    // This case tests applying rules #2 and #3 described in
    // ReshardingOplogApplicationRules::_applyUpdate_inlock.
    //
    // Make sure a document with {_id: 0} exists in the output collection that does not belong to
    // this donor shard before applying the updates.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << -1))));

        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(
            opCtx.get(),
            makeUpdateOp(BSON("_id" << 0),
                         update_oplog_entry::makeDeltaOplogEntry(
                             BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 1))))));
    }

    // The document {_id: 0, sk: -1} that exists in the output collection does not belong to this
    // donor shard, so we should have applied rule #3 and done nothing and the document should still
    // be in the output collection.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(
            opCtx.get(),
            makeUpdateOp(BSON("_id" << 2),
                         update_oplog_entry::makeDeltaOplogEntry(
                             BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 1))))));
    }

    // There does not exist a document with {_id: 2} in the output collection, so we should have
    // applied rule #2 and done nothing.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, UpdateOpModifiesOutputCollection) {
    // This case tests applying rule #4 described in
    // ReshardingOplogApplicationRules::_applyUpdate_inlock.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 1 << sk() << 1))));
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 2 << sk() << 2))));

        checkCollectionContents(opCtx.get(),
                                outputNss(),
                                {BSON("_id" << 1 << sk() << 1), BSON("_id" << 2 << sk() << 2)});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(
            opCtx.get(),
            makeUpdateOp(BSON("_id" << 1),
                         update_oplog_entry::makeDeltaOplogEntry(
                             BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 1))))));
        ASSERT_OK(applier()->applyOperation(
            opCtx.get(),
            makeUpdateOp(BSON("_id" << 2),
                         update_oplog_entry::makeDeltaOplogEntry(
                             BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 2))))));
    }

    // We should have updated both documents in the output collection to include the new field "x".
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(
            opCtx.get(),
            outputNss(),
            {BSON("_id" << 1 << sk() << 1 << "x" << 1), BSON("_id" << 2 << sk() << 2 << "x" << 2)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, DeleteOpRemovesFromStashCollectionAfterInsertConflict) {
    // This case tests applying rule #1 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    //
    // Insert a document {_id: 0} in the output collection before applying the insert of document
    // with {_id: 0}. This will force the document {_id: 0, sk: 1} to be inserted to the stash
    // collection for this donor shard.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << -1))));
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << 2))));

        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {BSON("_id" << 0 << sk() << 2)});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeDeleteOp(BSON("_id" << 0))));
    }

    // We should have applied rule #1 and deleted the document with {_id: 0} from the stash
    // collection for this donor. And the output collection should remain unchanged.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, DeleteOpIsNoopWhenDifferentOwningDonorOrNotMatching) {
    // This case tests applying rules #2 and #3 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    //
    // Make sure a document with {_id: 0} exists in the output collection that does not belong to
    // this donor shard before applying the deletes.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << -1))));

        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeDeleteOp(BSON("_id" << 0))));
    }

    // The document {_id: 0, sk: -1} that exists in the output collection does not belong to this
    // donor shard, so we should have applied rule #3 and done nothing and the document should still
    // be in the output collection.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeDeleteOp(BSON("_id" << 2))));
    }

    // There does not exist a document with {_id: 2} in the output collection, so we should have
    // applied rule #2 and done nothing.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -1)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, DeleteOpRemovesFromOutputCollection) {
    // This case tests applying rule #4 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 1 << sk() << 1))));
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 2 << sk() << 2))));

        checkCollectionContents(opCtx.get(),
                                outputNss(),
                                {BSON("_id" << 1 << sk() << 1), BSON("_id" << 2 << sk() << 2)});
    }

    const auto beforeDeleteOpTime = repl::ReplClientInfo::forClient(*getClient()).getLastOp();

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeDeleteOp(BSON("_id" << 1))));
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeDeleteOp(BSON("_id" << 2))));
    }

    // None of the stash collections have documents with _id == [op _id], so we should not have
    // found any documents to insert into the output collection with either {_id: 1} or {_id: 2}.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }

    // Assert that the delete on the output collection was run in a transaction by looking in the
    // oplog for an applyOps entry with a "d" op on the output collection.
    {
        auto opCtx = makeOperationContext();
        auto applyOpsInfo = findApplyOpsNewerThan(opCtx.get(), beforeDeleteOpTime.getTimestamp());
        ASSERT_EQ(applyOpsInfo.size(), 2U);
        for (size_t i = 0; i < applyOpsInfo.size(); ++i) {
            ASSERT_EQ(applyOpsInfo[i].operations.size(), 1U);
            ASSERT_EQ(OpType_serializer(applyOpsInfo[i].operations[0].getOpType()),
                      OpType_serializer(repl::OpTypeEnum::kDelete));
            ASSERT_EQ(applyOpsInfo[i].operations[0].getNss(), outputNss());
            ASSERT_BSONOBJ_BINARY_EQ(applyOpsInfo[i].operations[0].getObject()["_id"].wrap(),
                                     BSON("_id" << int32_t(i + 1)));
        }
    }
}

TEST_F(ReshardingOplogCrudApplicationTest, DeleteOpAtomicallyMovesFromOtherStashCollection) {
    // This case tests applying rule #4 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    //
    // Make sure a document with {_id: 0} exists in the stash collection for the other donor shard.
    // The stash collection for this donor shard is empty.
    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(
            applier()->applyOperation(opCtx.get(), makeInsertOp(BSON("_id" << 0 << sk() << 2))));

        {
            auto otherStashColl = acquireCollection(
                opCtx.get(),
                CollectionAcquisitionRequest::fromOpCtx(
                    opCtx.get(), otherStashNss(), AcquisitionPrerequisites::kWrite),
                MODE_IX);
            WriteUnitOfWork wuow(opCtx.get());
            ASSERT_OK(
                collection_internal::insertDocument(opCtx.get(),
                                                    otherStashColl.getCollectionPtr(),
                                                    InsertStatement{BSON("_id" << 0 << sk() << -3)},
                                                    nullptr /* opDebug */));
            wuow.commit();
        }

        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << 2)});
        checkCollectionContents(opCtx.get(), otherStashNss(), {BSON("_id" << 0 << sk() << -3)});
    }

    const auto beforeDeleteOpTime = repl::ReplClientInfo::forClient(*getClient()).getLastOp();

    {
        auto opCtx = makeOperationContext();
        ASSERT_OK(applier()->applyOperation(opCtx.get(), makeDeleteOp(BSON("_id" << 0))));
    }

    // We should have applied rule #4 and deleted the document that was in the output collection
    // {_id: 0, sk: 2}, deleted the document with the same _id {_id: 0, sk: -3} in the other donor
    // shard's stash collection and inserted this document into the output collection.
    {
        auto opCtx = makeOperationContext();
        checkCollectionContents(opCtx.get(), outputNss(), {BSON("_id" << 0 << sk() << -3)});
        checkCollectionContents(opCtx.get(), myStashNss(), {});
        checkCollectionContents(opCtx.get(), otherStashNss(), {});
    }

    // Assert that the delete on the output collection was run in a transaction by looking in the
    // oplog for an applyOps entry with the following ops:
    //   (1) op="d" on the output collection,
    //   (2) op="d" on the other stash namespace, and
    //   (3) op="i" on the output collection.
    {
        auto opCtx = makeOperationContext();
        auto applyOpsInfo = findApplyOpsNewerThan(opCtx.get(), beforeDeleteOpTime.getTimestamp());
        ASSERT_EQ(applyOpsInfo.size(), 1U);
        ASSERT_EQ(applyOpsInfo[0].operations.size(), 3U);

        ASSERT_EQ(OpType_serializer(applyOpsInfo[0].operations[0].getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kDelete));
        ASSERT_EQ(applyOpsInfo[0].operations[0].getNss(), outputNss());
        ASSERT_BSONOBJ_BINARY_EQ(applyOpsInfo[0].operations[0].getObject()["_id"].wrap(),
                                 BSON("_id" << 0));

        ASSERT_EQ(OpType_serializer(applyOpsInfo[0].operations[1].getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kDelete));
        ASSERT_EQ(applyOpsInfo[0].operations[1].getNss(), otherStashNss());
        ASSERT_BSONOBJ_BINARY_EQ(applyOpsInfo[0].operations[1].getObject()["_id"].wrap(),
                                 BSON("_id" << 0));

        ASSERT_EQ(OpType_serializer(applyOpsInfo[0].operations[2].getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kInsert));
        ASSERT_EQ(applyOpsInfo[0].operations[2].getNss(), outputNss());
        ASSERT_BSONOBJ_BINARY_EQ(applyOpsInfo[0].operations[2].getObject(),
                                 BSON("_id" << 0 << sk() << -3));
    }
}

}  // namespace
}  // namespace mongo
