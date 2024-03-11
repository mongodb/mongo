/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <benchmark/benchmark.h>

#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_op_observer.h"
#include "mongo/db/catalog/database_holder_impl.h"
#include "mongo/db/catalog/database_holder_mock.h"
#include "mongo/db/op_observer/change_stream_pre_images_op_observer.h"
#include "mongo/db/op_observer/fallback_op_observer.h"
#include "mongo/db/op_observer/fcv_op_observer.h"
#include "mongo/db/op_observer/find_and_modify_images_op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/op_observer/operation_logger_transaction_proxy.h"
#include "mongo/db/op_observer/user_write_block_mode_op_observer.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replication_consistency_markers_gen.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/shard_merge_recipient_op_observer.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/repl/tenant_migration_donor_op_observer.h"
#include "mongo/db/repl/tenant_migration_recipient_op_observer.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"
#include "mongo/db/s/query_analysis_op_observer_configsvr.h"
#include "mongo/db/s/query_analysis_op_observer_rs.h"
#include "mongo/db/s/query_analysis_op_observer_shardsvr.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/serverless/shard_split_donor_op_observer.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/timeseries/timeseries_op_observer.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/cluster_server_parameter_op_observer.h"
#include "mongo/logv2/log_domain_global.h"

namespace mongo {
namespace {
using namespace mongo::repl;
MONGO_INITIALIZER_GENERAL(CoreOptions_Store, (), ())
(InitializerContext* context) {
    // Dummy initializer to fill in the initializer graph
}

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

    ~ReplicationConsistencyMarkersBm() {
        tearDown();
    }

private:
    void _doTest() override{};

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

void setUpObservers(ServiceContext* serviceContext, ClusterRole clusterRole, bool isServerless) {
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
        if (isServerless) {
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationRecipientOpObserver>());
            opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::ShardMergeRecipientOpObserver>());
        }
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
        if (isServerless) {
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::TenantMigrationRecipientOpObserver>());
            opObserverRegistry->addObserver(std::make_unique<ShardSplitDonorOpObserver>());
            opObserverRegistry->addObserver(
                std::make_unique<repl::ShardMergeRecipientOpObserver>());
        }
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
    serviceContext->setOpObserver(std::move(opObserverRegistry));
}

void BM_setOplogTruncateAfterPoint(benchmark::State& state) {
    ReplicationConsistencyMarkersBm bmTest;
    StorageInterfaceImpl storage;
    ReplicationConsistencyMarkersImplMock consistencyMarkers(&storage);
    auto opCtx = bmTest.getOperationContext();
    auto serviceContext = bmTest.getServiceContext();

    setUpObservers(serviceContext, ClusterRole::None, false /* not serverless */);

    auto status = consistencyMarkers.createInternalCollections(opCtx);
    for (auto _ : state) {
        consistencyMarkers.setOplogTruncateAfterPoint(opCtx, Timestamp());
    }
}

BENCHMARK(BM_setOplogTruncateAfterPoint)->MinTime(10.0);
}  // namespace
}  // namespace mongo
