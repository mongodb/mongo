// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/replication_consistency_markers_impl.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_op_observer.h"
#include "mongo/db/op_observer/change_stream_pre_images_op_observer.h"
#include "mongo/db/op_observer/fallback_op_observer.h"
#include "mongo/db/op_observer/fcv_op_observer.h"
#include "mongo/db/op_observer/find_and_modify_images_op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/op_observer/operation_logger_transaction_proxy.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"
#include "mongo/db/s/query_analysis_op_observer_configsvr.h"
#include "mongo/db/s/query_analysis_op_observer_rs.h"
#include "mongo/db/s/query_analysis_op_observer_shardsvr.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/config_server_op_observer.h"
#include "mongo/db/sharding_environment/shard_server_op_observer.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/timeseries/timeseries_op_observer.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_op_observer.h"
#include "mongo/db/topology/user_write_block/replica_set_write_block_op_observer.h"
#include "mongo/db/topology/user_write_block/user_write_block_mode_op_observer.h"
#include "mongo/logv2/log_domain_global.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {
using namespace mongo::repl;

MONGO_INITIALIZER_GENERAL(DisableLogging, (), ())
(InitializerContext*) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

class StorageInterfaceMockTimestamp : public StorageInterfaceImpl {
public:
    boost::optional<OpTimeAndWallTime> findOplogOpTimeLessThanOrEqualToTimestampRetryOnWCE(
        OperationContext* opCtx, const CollectionPtr& oplog, const Timestamp& timestamp) override {
        auto now = Date_t::now();
        return OpTimeAndWallTime(OpTime(Timestamp(now), int64_t{5}), now);
    }

    Status upsertById(OperationContext* opCtx,
                      const NamespaceStringOrUUID& nsOrUUID,
                      const BSONElement& idKey,
                      const BSONObj& update) override {
        return Status::OK();
    }
};

class ReplicationConsistencyMarkersImplMock : public ReplicationConsistencyMarkersImpl {
public:
    ReplicationConsistencyMarkersImplMock(StorageInterface* storage)
        : ReplicationConsistencyMarkersImpl(storage) {}

    Timestamp getOplogTruncateAfterPoint(OperationContext* opCtx) const override {
        return Timestamp(3, 5);
    }
};

class ReplicationConsistencyMarkersBm : public ServiceContextMongoDTest {
public:
    ReplicationConsistencyMarkersBm() : ServiceContextMongoDTest() {
        setUp();
    }

    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

    ~ReplicationConsistencyMarkersBm() override {
        tearDown();
    }

private:
    void TestBody() override {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = cc().makeOperationContext();
        auto replCoord = std::make_unique<ReplicationCoordinatorMock>(getServiceContext());
        ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
    }

    void tearDown() override {
        _opCtx.reset(nullptr);
        ServiceContextMongoDTest::tearDown();
    }

    ServiceContext::UniqueOperationContext _opCtx;
    FailPointEnableBlock _skipDirectConnectionChecks{"skipDirectConnectionChecks"};
};

void BM_refreshOplogTruncateAFterPointIfPrimary(benchmark::State& state) {
    ReplicationConsistencyMarkersBm bmTest;
    StorageInterfaceMockTimestamp storage;
    ReplicationConsistencyMarkersImplMock consistencyMarkers(&storage);
    auto opCtx = bmTest.getOperationContext();
    consistencyMarkers.startUsingOplogTruncateAfterPointForPrimary();

    auto status = consistencyMarkers.createInternalCollections(opCtx);
    for (auto _ : state) {
        consistencyMarkers.refreshOplogTruncateAfterPointIfPrimary(opCtx);
    }
    consistencyMarkers.stopUsingOplogTruncateAfterPointForPrimary();
}

BENCHMARK(BM_refreshOplogTruncateAFterPointIfPrimary)->MinTime(10.0);

void setUpObservers(ServiceContext* serviceContext, ClusterRole clusterRole) {
    auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
    if (clusterRole.has(ClusterRole::ShardServer)) {
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerTransactionProxy>(
                std::make_unique<OperationLoggerImpl>())));
        opObserverRegistry->addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<MigrationChunkClonerSourceOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ShardServerOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<UserWriteBlockModeOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReplicaSetWriteBlockOpObserver>());

        if (!gMultitenancySupport) {
            opObserverRegistry->addObserver(
                std::make_unique<analyze_shard_key::QueryAnalysisOpObserverShardSvr>());
        }
    }

    if (clusterRole.has(ClusterRole::ConfigServer)) {
        opObserverRegistry->addObserver(std::make_unique<ConfigServerOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReshardingOpObserver>());
        if (!gMultitenancySupport) {
            opObserverRegistry->addObserver(
                std::make_unique<analyze_shard_key::QueryAnalysisOpObserverConfigSvr>());
        }
    }

    if (clusterRole.has(ClusterRole::None)) {
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
        opObserverRegistry->addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<UserWriteBlockModeOpObserver>());
        opObserverRegistry->addObserver(std::make_unique<ReplicaSetWriteBlockOpObserver>());

        if (!gMultitenancySupport) {
            opObserverRegistry->addObserver(
                std::make_unique<analyze_shard_key::QueryAnalysisOpObserverRS>());
        }
    }

    opObserverRegistry->addObserver(std::make_unique<FallbackOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<TimeSeriesOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<AuthOpObserver>());
    opObserverRegistry->addObserver(
        std::make_unique<repl::PrimaryOnlyServiceOpObserver>(serviceContext));
    opObserverRegistry->addObserver(std::make_unique<FcvOpObserver>());
    opObserverRegistry->addObserver(std::make_unique<ClusterServerParameterOpObserver>());

    if (audit::opObserverRegistrar) {
        audit::opObserverRegistrar(opObserverRegistry.get());
    }
    serviceContext->resetOpObserver_forTest(std::move(opObserverRegistry));
}

void BM_setOplogTruncateAfterPoint(benchmark::State& state) {
    ReplicationConsistencyMarkersBm bmTest;
    StorageInterfaceImpl storage;
    ReplicationConsistencyMarkersImplMock consistencyMarkers(&storage);
    auto opCtx = bmTest.getOperationContext();
    auto serviceContext = bmTest.getServiceContext();

    setUpObservers(serviceContext, ClusterRole::None);

    auto status = consistencyMarkers.createInternalCollections(opCtx);
    for (auto _ : state) {
        consistencyMarkers.setOplogTruncateAfterPoint(opCtx, Timestamp());
    }
}

BENCHMARK(BM_setOplogTruncateAfterPoint)->MinTime(10.0);
}  // namespace
}  // namespace mongo
