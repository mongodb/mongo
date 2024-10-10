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
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/op_observer/change_stream_pre_images_op_observer.h"
#include "mongo/db/op_observer/fallback_op_observer.h"
#include "mongo/db/op_observer/fcv_op_observer.h"
#include "mongo/db/op_observer/find_and_modify_images_op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/op_observer/operation_logger_transaction_proxy.h"
#include "mongo/db/op_observer/user_write_block_mode_op_observer.h"
#include "mongo/db/repl/primary_only_service_op_observer.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/tenant_migration_donor_op_observer.h"
#include "mongo/db/repl/tenant_migration_recipient_op_observer.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"
#include "mongo/db/s/query_analysis_op_observer_configsvr.h"
#include "mongo/db/s/query_analysis_op_observer_rs.h"
#include "mongo/db/s/query_analysis_op_observer_shardsvr.h"
#include "mongo/db/s/resharding/resharding_op_observer.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/timeseries/timeseries_op_observer.h"
#include "mongo/idl/cluster_server_parameter_op_observer.h"
#include "mongo/logv2/log_domain_global.h"

namespace mongo {
namespace {

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

ServiceContext* setupServiceContext() {
    auto serviceContext = ServiceContext::make();
    auto serviceContextPtr = serviceContext.get();
    setGlobalServiceContext(std::move(serviceContext));
    return serviceContextPtr;
}

void setUpObservers(ServiceContext* serviceContext,
                    OpObserverRegistry* opObserverRegistry,
                    ClusterRole clusterRole,
                    bool isServerless) {
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
        }
        if (!gMultitenancySupport) {  // && replCoord && replCoord->getSettings().isReplSet()) {
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
        audit::opObserverRegistrar(opObserverRegistry);
    }
}

void BM_OnUpdate(benchmark::State& state, const char* nss) {
    CollectionMock coll(NamespaceString::createNamespaceString_forTest(nss));
    CollectionPtr collptr(&coll);
    CollectionUpdateArgs cuArgs(BSON("_id"
                                     << "whatever"
                                     << "stmtid"
                                     << "oldstuff"));
    cuArgs.update = BSON("_id"
                         << "whatever"
                         << "stmtid"
                         << "whateverelse");
    OplogUpdateEntryArgs args(&cuArgs, collptr);
    OpObserverRegistry registry;
    auto* serviceContext = setupServiceContext();
    repl::ReplSettings replSettings;
    replSettings.setReplSetString("rs0/host1");
    repl::ReplicationCoordinator::set(
        serviceContext,
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext, replSettings));

    auto client = serviceContext->getService()->makeClient("BM_OnUpdate_Client");
    auto opCtx = client->makeOperationContext();
    repl::UnreplicatedWritesBlock uwb(opCtx.get());
    setUpObservers(serviceContext, &registry, ClusterRole::None, false /* not serverless */);
    for (auto _ : state) {
        registry.onUpdate(opCtx.get(), args);
    }
}

void BM_OnUpdate_ConfigTransactions(benchmark::State& state) {
    BM_OnUpdate(state, "config.transactions");
}

void BM_OnUpdate_User(benchmark::State& state) {
    BM_OnUpdate(state, "test.coll1");
}

void BM_OnUpdate_System(benchmark::State& state) {
    BM_OnUpdate(state, "test.system.special");
}

BENCHMARK(BM_OnUpdate_ConfigTransactions)->MinTime(10.0);
BENCHMARK(BM_OnUpdate_User)->MinTime(10.0);
BENCHMARK(BM_OnUpdate_System)->MinTime(10.0);

void BM_OnInserts(benchmark::State& state, const char* nss) {
    CollectionMock coll(NamespaceString::createNamespaceString_forTest(nss));
    CollectionPtr collptr(&coll);
    std::vector<InsertStatement> statements(1,
                                            InsertStatement(BSON("_id"
                                                                 << "whatever"
                                                                 << "key"
                                                                 << "value")));
    OpObserverRegistry registry;
    auto* serviceContext = setupServiceContext();
    repl::ReplSettings replSettings;
    replSettings.setReplSetString("rs0/host1");
    repl::ReplicationCoordinator::set(
        serviceContext,
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext, replSettings));

    auto client = serviceContext->getService()->makeClient("BM_OnInserts_Client");
    auto opCtx = client->makeOperationContext();
    repl::UnreplicatedWritesBlock uwb(opCtx.get());
    setUpObservers(serviceContext, &registry, ClusterRole::None, false /* not serverless */);
    for (auto _ : state) {
        registry.onInserts(opCtx.get(),
                           collptr,
                           statements.cbegin(),
                           statements.cend(),
                           {} /* recordIds */,
                           {false},
                           false,
                           nullptr);
    }
}

void BM_OnInserts_ConfigTransactions(benchmark::State& state) {
    BM_OnInserts(state, "config.transactions");
}

void BM_OnInserts_User(benchmark::State& state) {
    BM_OnInserts(state, "test.coll1");
}

void BM_OnInserts_System(benchmark::State& state) {
    BM_OnInserts(state, "test.system.special");
}

BENCHMARK(BM_OnInserts_ConfigTransactions)->MinTime(10.0);
BENCHMARK(BM_OnInserts_User)->MinTime(10.0);
BENCHMARK(BM_OnInserts_System)->MinTime(10.0);

void BM_onDelete(benchmark::State& state, const char* nss) {
    CollectionMock coll(NamespaceString::createNamespaceString_forTest(nss));
    CollectionPtr collptr(&coll);
    BSONObj const doc = BSON("_id"
                             << "whatever"
                             << "key"
                             << "value");
    const auto& documentKey = getDocumentKey(collptr, doc);
    OplogDeleteEntryArgs deleteArgs;
    OpObserverRegistry registry;
    auto* serviceContext = setupServiceContext();
    repl::ReplSettings replSettings;
    replSettings.setReplSetString("rs0/host1");
    repl::ReplicationCoordinator::set(
        serviceContext,
        std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext, replSettings));

    auto client = serviceContext->getService()->makeClient("BM_onDelete_Client");
    auto opCtx = client->makeOperationContext();
    repl::UnreplicatedWritesBlock uwb(opCtx.get());

    setUpObservers(serviceContext, &registry, ClusterRole::None, false /* not serverless */);
    for (auto _ : state) {
        registry.onDelete(
            opCtx.get(), collptr, kUninitializedStmtId, doc, documentKey, deleteArgs, nullptr);
    }
}

void BM_onDelete_ConfigTransactions(benchmark::State& state) {
    BM_onDelete(state, "config.transactions");
}

void BM_onDelete_User(benchmark::State& state) {
    BM_onDelete(state, "test.coll1");
}

void BM_onDelete_System(benchmark::State& state) {
    BM_onDelete(state, "test.system.special");
}

BENCHMARK(BM_onDelete_ConfigTransactions)->MinTime(10.0);
BENCHMARK(BM_onDelete_User)->MinTime(10.0);
BENCHMARK(BM_onDelete_System)->MinTime(10.0);

}  // namespace
}  // namespace mongo
