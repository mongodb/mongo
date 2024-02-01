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

#include "mongo/db/s/shard_server_op_observer.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/replica_set_endpoint_sharding_state.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/sharding_state.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using replica_set_endpoint::ReplicaSetEndpointShardingState;

class ShardServerOpObserverReplicaSetEndpointTest : public ServiceContextMongoDTest {
protected:
    explicit ShardServerOpObserverReplicaSetEndpointTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

    virtual void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

        // Set up the ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, _createReplSettings()));
        repl::createOplog(opCtx);
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        reset(opCtx, NamespaceString::kConfigsvrShardsNamespace);
        reset(opCtx, NamespaceString::kRsOplogNamespace);
    }

    void reset(OperationContext* opCtx,
               NamespaceString nss,
               boost::optional<UUID> uuid = boost::none) const {
        writeConflictRetry(opCtx, "deleteAll", nss, [&] {
            shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collRaii(opCtx, nss, MODE_X);
            if (collRaii) {
                invariant(collRaii.getWritableCollection(opCtx)->truncate(opCtx).isOK());
            } else {
                auto db = collRaii.ensureDbExists(opCtx);
                CollectionOptions opts;
                if (uuid) {
                    opts.uuid = uuid;
                }
                invariant(db->createCollection(opCtx, nss, opts));
            }
            wunit.commit();
        });
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

    BSONObj makeConfigShardsDoc(std::string shardId) {
        ShardType shardDoc;
        shardDoc.setName(shardId);
        shardDoc.setHost(shardId + "Node" + ":12345");
        shardDoc.setState(ShardType::ShardState::kShardAware);
        return shardDoc.toBSON();
    }

    void insertDocuments(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const std::vector<BSONObj>& docs) {
        ShardServerOpObserver opObserver;

        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection coll(opCtx, nss, MODE_IX);

        std::vector<InsertStatement> insert;
        for (auto& doc : docs) {
            insert.emplace_back(doc);
        }
        opObserver.onInserts(opCtx,
                             *coll,
                             insert.begin(),
                             insert.end(),
                             /*recordIds*/ {},
                             /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                             /*defaultFromMigrate=*/false);
        wuow.commit();
    }

    void insertToConfigShards(OperationContext* opCtx, const std::vector<std::string>& shardIds) {
        std::vector<BSONObj> docs;
        for (auto& shardId : shardIds) {
            docs.push_back(makeConfigShardsDoc(shardId));
        }
        insertToConfigShards(opCtx, docs);
    }

    void insertToConfigShards(OperationContext* opCtx, const std::vector<BSONObj>& docs) {
        insertDocuments(opCtx, NamespaceString::kConfigsvrShardsNamespace, docs);
    }

    void deleteDocument(OperationContext* opCtx, const NamespaceString& nss, const BSONObj doc) {
        ShardServerOpObserver opObserver;

        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection coll(opCtx, nss, MODE_IX);

        OplogDeleteEntryArgs args;
        opObserver.aboutToDelete(opCtx, *coll, doc, &args);
        opObserver.onDelete(opCtx, *coll, kUninitializedStmtId, doc, args);

        wuow.commit();
    }

    void deleteFromConfigShards(OperationContext* opCtx, std::string shardId) {
        auto doc = makeConfigShardsDoc(shardId);
        deleteFromConfigShards(opCtx, doc);
    }

    void deleteFromConfigShards(OperationContext* opCtx, const BSONObj& doc) {
        deleteDocument(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc);
    }

    void updateDocument(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj preImageDoc) {
        ShardServerOpObserver opObserver;

        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection coll(opCtx, nss, MODE_IX);

        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.criteria = preImageDoc;
        auto mod = BSON("$set" << BSON("updated" << true));
        BSONObjBuilder bob(preImageDoc);
        bob.appendElements(updateArgs.update);
        updateArgs.update = bob.obj();  // replacement update.
        updateArgs.updatedDoc = updateArgs.update;
        OplogUpdateEntryArgs update(&updateArgs, *coll);
        opObserver.onUpdate(opCtx, update);

        wuow.commit();
    }

    void updateConfigShards(OperationContext* opCtx, std::string shardId) {
        auto doc = makeConfigShardsDoc(shardId);
        updateConfigShards(opCtx, doc);
    }

    void updateConfigShards(OperationContext* opCtx, const BSONObj& doc) {
        updateDocument(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc);
    }

private:
    repl::ReplSettings _createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString(_setName + "/" + _hostName);
        return settings;
    }

    StringData _setName = "testSet";
    StringData _hostName = "testNode1:12345";
    RAIIServerParameterControllerForTest _replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", true};
};

TEST_F(ShardServerOpObserverReplicaSetEndpointTest, ConfigShardsOnInserts_NotConfigShard) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    insertToConfigShards(opCtx, {UUID::gen().toString()});
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnInserts_ConfigShard_OnConfigServer_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    insertToConfigShards(opCtx, {ShardId::kConfigServerId.toString()});
    ASSERT(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnInserts_ConfigShard_OnConfigServer_FeatureFlagDisabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    RAIIServerParameterControllerForTest replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", false};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    insertToConfigShards(opCtx, {ShardId::kConfigServerId.toString()});
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnInserts_ConfigShard_NotOnConfigServer_FeatureFlagEnabled) {
    // The config.shards collection should only exist on the config server but testing here for
    // completion.
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    insertToConfigShards(opCtx, {ShardId::kConfigServerId.toString()});
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardOnDelete_ConfigShard_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());
    deleteFromConfigShards(opCtx, ShardId::kConfigServerId.toString());
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardOnDelete_ConfigShard_FeatureFlagDisabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    RAIIServerParameterControllerForTest replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", false};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());
    deleteFromConfigShards(opCtx, ShardId::kConfigServerId.toString());
    ASSERT(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnDelete_NotConfigShard_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());
    deleteFromConfigShards(opCtx, UUID::gen().toString());
    ASSERT(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnUpdate_DoesNotUnsetIsConfigShard_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());
    updateConfigShards(opCtx, {ShardId::kConfigServerId.toString()});
    ASSERT(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnUpdate_DoesNotSetIsConfigShard_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    updateConfigShards(opCtx, {ShardId::kConfigServerId.toString()});
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnInserts_InvalidDoc_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    insertToConfigShards(opCtx, {BSON("_id" << UUID::gen())});  // Not ShardType.
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnDelete_InvalidDoc_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());
    deleteFromConfigShards(opCtx, BSON("_id" << UUID::gen()));  // Not ShardType.
    ASSERT(shardingState->isConfigShardForTest());
}

TEST_F(ShardServerOpObserverReplicaSetEndpointTest,
       ConfigShardsOnUpdate_InvalidDoc_FeatureFlagEnabled) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto opCtxHolder = cc().makeOperationContext();
    auto opCtx = opCtxHolder.get();
    auto shardingState = ReplicaSetEndpointShardingState::get(opCtx);

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    updateConfigShards(opCtx, BSON("_id" << UUID::gen()));  // Not ShardType.
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

}  // namespace
}  // namespace mongo
