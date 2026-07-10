/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/commands/set_feature_compatibility_version_steps/fcv_step.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/ddl/create_indexes_gen.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/shard_role/ddl/drop_indexes_gen.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Generates values for the 'uuid' field of each document of 'config.shards', and then applies those
 * values to the shard identity document on each shard. Already existing 'uuids' values (which may
 * have been generated in a previous call of this function) are preserved.
 * This method is meant to be exclusively called by the config server (and implies the submission of
 * remote commands to each shard).
 */
void generateShardUUIDMetadata(OperationContext* opCtx) {
    // Ensure stable topology throughout the execution of this method against concurrent removeShard
    // requests.
    DDLLockManager::ScopedCollectionDDLLock ddlLock(opCtx,
                                                    NamespaceString::kConfigsvrShardsNamespace,
                                                    "generateShardUUIDMetadata",
                                                    LockMode::MODE_IX);
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    const auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    auto isConfigServerDedicated = true;

    // 1. Generate 'uuid' values for each document of 'config.shards' that does not have one yet).
    if (!shardIds.empty()) {
        std::vector<write_ops::UpdateOpEntry> insertUuidStmts;
        insertUuidStmts.reserve(shardIds.size());
        for (const auto& shardId : shardIds) {
            const auto shardUuid =
                (shardId == ShardId::kConfigServerId) ? ShardType::kConfigServerUuid : UUID::gen();
            write_ops::UpdateOpEntry updateStmt;
            updateStmt.setQ(BSON(ShardType::name.name()
                                 << shardId.toString() << ShardType::uuid.name()
                                 << BSON("$exists" << false)));
            updateStmt.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                BSON("$set" << BSON(ShardType::uuid.name() << shardUuid))));
            updateStmt.setUpsert(false);
            updateStmt.setMulti(false);
            insertUuidStmts.push_back(std::move(updateStmt));
            if (shardId == ShardId::kConfigServerId) {
                // Annotate for later consumption.
                isConfigServerDedicated = false;
            }
        }

        write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigsvrShardsNamespace);
        updateOp.setUpdates(std::move(insertUuidStmts));

        // Run the sequence of updates in a transaction. If at least one document is updated,
        // bump one of the 'topologyTime' fields to advance the related vector clock component.
        const auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                          ExecutorPtr /*txnExec*/) {
            const auto result = txnClient.runCRUDOpSync(BatchedCommandRequest(updateOp), {});
            uassertStatusOK(result.toStatus());

            if (result.getNModified() > 0) {
                const auto topologyTime =
                    VectorClockMutable::get(opCtx)->tickClusterTime(1).asTimestamp();
                write_ops::UpdateOpEntry topologyTimeEntry;
                topologyTimeEntry.setQ(BSON(ShardType::name.name() << shardIds.back().toString()));
                topologyTimeEntry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                    BSON("$set" << BSON(ShardType::topologyTime.name() << topologyTime))));
                topologyTimeEntry.setUpsert(false);
                topologyTimeEntry.setMulti(false);

                write_ops::UpdateCommandRequest topologyTimeOp(
                    NamespaceString::kConfigsvrShardsNamespace);
                topologyTimeOp.setUpdates({topologyTimeEntry});
                const auto topologyTimeResult =
                    txnClient.runCRUDOpSync(BatchedCommandRequest(topologyTimeOp), {});
                uassertStatusOK(topologyTimeResult.toStatus());
            }
            LOGV2(12696400,
                  "Successfully generated 'uuid' fields for 'config.shards'",
                  "numUpdated"_attr = result.getNModified());

            return SemiFuture<void>::makeReady();
        };

        auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
        txn_api::SyncTransactionWithRetries txn(opCtx,
                                                executor,
                                                nullptr, /*resourceYielder*/
                                                inlineExecutor);
        txn.run(opCtx, transactionChain);
    }

    // 2. Create the 'uuid' index on 'config.shards'.
    DBDirectClient directClient(opCtx);
    CreateIndexesCommand createIndexesCmd(NamespaceString::kConfigsvrShardsNamespace);
    createIndexesCmd.setCommitQuorum(CommitQuorumOptions(CommitQuorumOptions::kMajority));
    createIndexesCmd.setIndexes({BSON("key" << BSON(ShardType::uuid() << 1) << "name"
                                            << "uuid_1"
                                            << "unique" << true)});

    BSONObj res;
    directClient.runCommand(
        NamespaceString::kConfigsvrShardsNamespace.dbName(), createIndexesCmd.toBSON(), res);
    uassertStatusOK(getStatusFromCommandResult(res));

    // 3. Persist 'uuid' values into each shard's shard identity document.
    const auto opTimeWithShards =
        ShardingCatalogManager::get(opCtx)->localCatalogClient()->getAllShards(
            opCtx, repl::ReadConcernArgs::kLocal);

    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardType : opTimeWithShards.value) {
        const auto& uuid = shardType.getUuid();
        tassert(12696403,
                str::stream() << "Missing 'uuid' field from shard " << shardType.getName(),
                uuid);

        write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON("_id" << ShardIdentityType::IdName));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(ShardType::uuid.name() << *uuid))));
        entry.setUpsert(false);
        entry.setMulti(false);
        updateOp.setUpdates({entry});
        updateOp.setWriteConcern(defaultMajorityWriteConcern());
        requests.emplace_back(ShardRef(shardType.getName()), updateOp.toBSON());
    }

    if (!requests.empty()) {
        AsyncRequestsSender ars(opCtx,
                                executor,
                                DatabaseName::kAdmin,
                                requests,
                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                Shard::RetryPolicy::kIdempotent,
                                nullptr /* resourceYielder */,
                                {} /* designatedHostsMap */);

        while (!ars.done()) {
            auto response = ars.next();
            uassertStatusOKWithContext(AsyncRequestsSender::Response::getEffectiveStatus(response),
                                       str::stream()
                                           << "Failed to update ShardIdentity uuid on shard '"
                                           << response.shardId << "'");
        }
    }

    // Dedicated Config servers require special treatment, since they do not have a config.shards
    // document to read the 'uuid' value from.
    if (isConfigServerDedicated) {
        write_ops::UpdateCommandRequest configIdentityUpdateOp(
            NamespaceString::kServerConfigurationNamespace);
        write_ops::UpdateOpEntry configIdentityEntry;
        configIdentityEntry.setQ(BSON("_id" << ShardIdentityType::IdName << ShardType::uuid.name()
                                            << BSON("$exists" << false)));
        configIdentityEntry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$set" << BSON(ShardType::uuid.name() << ShardType::kConfigServerUuid))));
        configIdentityEntry.setUpsert(false);
        configIdentityEntry.setMulti(false);
        configIdentityUpdateOp.setUpdates({configIdentityEntry});
        const auto configIdentityResult = directClient.update(configIdentityUpdateOp);
        write_ops::checkWriteErrors(configIdentityResult);
        LOGV2(12696404,
              "Successfully generated 'uuid' field for config server's shardIdentity document");
    }

    LOGV2(12696402, "Successfully updated 'uuid' field in ShardIdentity on all shards");
}

void removeUuidFieldFromShardIdentityDoc(OperationContext* opCtx) {
    DBDirectClient client{opCtx};

    write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << ShardIdentityType::IdName));
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
        BSON("$unset" << BSON(ShardType::uuid.name() << 1))));
    entry.setUpsert(false);
    entry.setMulti(false);
    updateOp.setUpdates({entry});
    write_ops::checkWriteErrors(client.update(updateOp));
}

void removeUuidMetadataFromConfigShards(OperationContext* opCtx) {
    DBDirectClient client{opCtx};

    // Drop the 'uuid' index on config.shards.
    try {
        DropIndexes dropIndexesCmd{NamespaceString::kConfigsvrShardsNamespace};
        dropIndexesCmd.setIndex(BSON(ShardType::uuid() << 1));
        BSONObj info;
        if (!client.runCommand(NamespaceString::kConfigsvrShardsNamespace.dbName(),
                               dropIndexesCmd.toBSON(),
                               info)) {
            uassertStatusOK(getStatusFromCommandResult(info));
        }
    } catch (const ExceptionFor<ErrorCodes::IndexNotFound>&) {
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
    }

    // Clear the shard UUID from each config.shards document.
    {
        write_ops::UpdateCommandRequest configShardsUpdateOp(
            NamespaceString::kConfigsvrShardsNamespace);
        write_ops::UpdateOpEntry configShardsEntry;
        configShardsEntry.setQ(BSON(ShardType::uuid.name() << BSON("$exists" << true)));
        configShardsEntry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$unset" << BSON(ShardType::uuid.name() << 1))));
        configShardsEntry.setUpsert(false);
        configShardsEntry.setMulti(true);
        configShardsUpdateOp.setUpdates({configShardsEntry});
        write_ops::checkWriteErrors(client.update(configShardsUpdateOp));
    }
}

/*
 * Implements the logic to "generate & persist" or "clear" UUID values for each existing shard in
 * the cluster during an FCV transition.
 */
class SetClearShardUuidsFCVStep : public FCVStep {
public:
    static SetClearShardUuidsFCVStep* get(ServiceContext* serviceContext);

    inline std::string getStepName() const final {
        return "SetClearShardUuidsFCVStep";
    }

private:
    void upgradeServerMetadata(OperationContext* opCtx,
                               FCV originalVersion,
                               FCV requestedVersion) final {
        if (!feature_flags::gFeatureFlagAssignUUIDToShard
                 .isEnabledOnTargetFCVButDisabledOnOriginalFCV(requestedVersion, originalVersion)) {
            return;
        }

        auto role = ShardingState::get(opCtx)->pollClusterRole();
        if (!role || !role->has(ClusterRole::ConfigServer)) {
            return;
        }

        generateShardUUIDMetadata(opCtx);
    }

    void internalServerCleanupForDowngrade(OperationContext* opCtx,
                                           FCV originalVersion,
                                           FCV requestedVersion) final {
        if (feature_flags::gFeatureFlagAssignUUIDToShard.isEnabledOnVersion(requestedVersion)) {
            return;
        }

        removeUuidFieldFromShardIdentityDoc(opCtx);

        if (const auto role = ShardingState::get(opCtx)->pollClusterRole();
            role && role->has(ClusterRole::ConfigServer)) {
            removeUuidMetadataFromConfigShards(opCtx);
        }
    }
};

const auto decoration = ServiceContext::declareDecoration<SetClearShardUuidsFCVStep>();
const FCVStepRegistry::Registerer<SetClearShardUuidsFCVStep> setClearShardUuidFCVStepRegisterer(
    "SetClearShardUuidsFCVStep");

SetClearShardUuidsFCVStep* SetClearShardUuidsFCVStep::get(ServiceContext* serviceContext) {
    return &decoration(serviceContext);
}

}  // namespace
}  // namespace mongo
