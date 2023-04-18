/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/sharding_index_catalog_util.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/sharded_index_catalog_commands_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace sharding_index_catalog_util {

namespace {

void performNoopRetryableWriteForIndexCommit(
    OperationContext* opCtx,
    OperationSessionInfo& osi,
    const std::set<ShardId>& shardIdSet,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    std::vector<ShardId> shardsAndConfigsvr{shardIdSet.begin(), shardIdSet.end()};
    if (std::find(shardsAndConfigsvr.begin(), shardsAndConfigsvr.end(), ShardId::kConfigServerId) ==
        shardsAndConfigsvr.end()) {
        // The config server may be a shard, so only add if it isn't already in shardsAndConfigsvr.
        shardsAndConfigsvr.push_back(Grid::get(opCtx)->shardRegistry()->getConfigShard()->getId());
    }
    sharding_ddl_util::performNoopRetryableWriteOnShards(opCtx, shardsAndConfigsvr, osi, executor);
    osi.setTxnNumber(++osi.getTxnNumber().get());
}

BSONObj getCriticalSectionReasonForIndexCommit(const NamespaceString& nss,
                                               const std::string& name) {
    return BSON("command"
                << "commitIndexCatalogEntry"
                << "nss" << nss.toStringForErrorMsg() << IndexCatalogType::kNameFieldName << name);
}

/**
 * Function with an stable vector of shardId's (meaning, migrations will be serialized with this
 * function call) that should perform catalog updates.
 */
using IndexModificationCallback = unique_function<void(std::vector<ShardId>&)>;

/**
 * Helper function to generalize the index catalog modification protocol. With this function when
 * callback is called, we have the following guarantees:
 *
 * 1. All migrations will be cancelled, will not be able to commit and will no new migration will
 * start for userCollectionNss.
 * 2. osi will contain a valid sessionID and transaction number, even after a stepdown.
 * 3. There won't be any writes for userCollectionNss because the critical section will be taken
 * cluster-wide.
 *
 * After the execution of this function, the migrations will be enabled again, unless the function
 * failed due to a step-down. In which case, this function should be called again on stepUp after
 * the node is in steady-state. osi will contain the latest txnNumber used.
 *
 * Any work done by callback must be resumable and idempotent.
 */
void coordinateIndexCatalogModificationAcrossCollectionShards(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    OperationSessionInfo& osi,
    const NamespaceString& userCollectionNss,
    const std::string& indexName,
    const UUID& collectionUUID,
    const bool firstExecution,
    IndexModificationCallback callback) {
    // Stop migrations so the cluster is in a steady state.
    sharding_ddl_util::stopMigrations(opCtx, userCollectionNss, collectionUUID);
    // Resume migrations no matter what.
    ON_BLOCK_EXIT(
        [&] { sharding_ddl_util::resumeMigrations(opCtx, userCollectionNss, collectionUUID); });

    // Get an up to date shard distribution.
    auto [routingInfo, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithPlacementRefresh(
            opCtx, userCollectionNss));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "collection " << userCollectionNss.toStringForErrorMsg()
                          << " is not sharded",
            routingInfo.isSharded());
    std::set<ShardId> shardIdsSet;
    routingInfo.getAllShardIds(&shardIdsSet);

    if (!firstExecution) {
        // If this is not the first execution (as in, there was a stepdown) advance the
        // txnNumber for this lsid, so requests with older txnNumbers can no longer execute.
        performNoopRetryableWriteForIndexCommit(opCtx, osi, shardIdsSet, executor);
    }

    std::vector<ShardId> shardIdsVec{shardIdsSet.begin(), shardIdsSet.end()};

    // Block writes in all shards that holds data for the user collection.
    ShardsvrParticipantBlock shardsvrBlockWritesRequest(userCollectionNss);
    shardsvrBlockWritesRequest.setBlockType(CriticalSectionBlockTypeEnum::kWrites);
    shardsvrBlockWritesRequest.setReason(
        getCriticalSectionReasonForIndexCommit(userCollectionNss, indexName));

    sharding_util::sendCommandToShards(
        opCtx,
        userCollectionNss.db(),
        CommandHelpers::appendMajorityWriteConcern(shardsvrBlockWritesRequest.toBSON({})),
        shardIdsVec,
        executor);

    // Perform the index modification.
    callback(shardIdsVec);

    // Release the critical section in all the shards.
    shardsvrBlockWritesRequest.setBlockType(CriticalSectionBlockTypeEnum::kUnblock);
    sharding_util::sendCommandToShards(
        opCtx,
        userCollectionNss.db(),
        CommandHelpers::appendMajorityWriteConcern(shardsvrBlockWritesRequest.toBSON({})),
        shardIdsVec,
        executor);
}

}  // namespace

void registerIndexCatalogEntry(OperationContext* opCtx,
                               std::shared_ptr<executor::TaskExecutor> executor,
                               OperationSessionInfo& osi,
                               const NamespaceString& userCollectionNss,
                               const std::string& name,
                               const BSONObj& keyPattern,
                               const BSONObj& options,
                               const UUID& collectionUUID,
                               const boost::optional<UUID>& indexCollectionUUID,
                               bool firstExecution) {
    coordinateIndexCatalogModificationAcrossCollectionShards(
        opCtx,
        executor,
        osi,
        userCollectionNss,
        name,
        collectionUUID,
        firstExecution,
        [&](std::vector<ShardId>& shardIds) {
            IndexCatalogType index;
            index.setCollectionUUID(collectionUUID);
            index.setIndexCollectionUUID(indexCollectionUUID);
            index.setKeyPattern(keyPattern);
            index.setLastmod([opCtx] {
                VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
                return vt.clusterTime().asTimestamp();
            }());
            index.setName(name);
            index.setOptions(options);

            ShardsvrCommitIndexParticipant shardsvrCommitIndexParticipantRequest(userCollectionNss);
            shardsvrCommitIndexParticipantRequest.setIndexCatalogType(index);
            shardsvrCommitIndexParticipantRequest.setDbName(DatabaseName::kAdmin);

            sharding_util::sendCommandToShards(
                opCtx,
                userCollectionNss.db(),
                CommandHelpers::appendMajorityWriteConcern(
                    shardsvrCommitIndexParticipantRequest.toBSON(osi.toBSON())),
                shardIds,
                executor);

            // Now commit the change in the config server.
            ConfigsvrCommitIndex configsvrCommitIndexRequest(userCollectionNss);
            configsvrCommitIndexRequest.setIndexCatalogType(index);
            configsvrCommitIndexRequest.setDbName(DatabaseName::kAdmin);
            auto commitIndexEntryResponse =
                Grid::get(opCtx)
                    ->shardRegistry()
                    ->getConfigShard()
                    ->runCommandWithFixedRetryAttempts(
                        opCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        "admin",
                        CommandHelpers::appendMajorityWriteConcern(
                            configsvrCommitIndexRequest.toBSON(osi.toBSON())),
                        Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(commitIndexEntryResponse));
        });
}

void unregisterIndexCatalogEntry(OperationContext* opCtx,
                                 std::shared_ptr<executor::TaskExecutor> executor,
                                 OperationSessionInfo& osi,
                                 const NamespaceString& userCollectionNss,
                                 const std::string& name,
                                 const UUID& collectionUUID,
                                 bool firstExecution) {
    coordinateIndexCatalogModificationAcrossCollectionShards(
        opCtx,
        executor,
        osi,
        userCollectionNss,
        name,
        collectionUUID,
        firstExecution,
        [&](std::vector<ShardId>& shardIdsVec) {
            // Remove the index in the config server.
            ConfigsvrDropIndexCatalogEntry configsvrDropIndexCatalogRequest(userCollectionNss);
            UnregisterIndexCatalogRequest dropIndexCatalogRequest;
            dropIndexCatalogRequest.setCollectionUUID(collectionUUID);
            dropIndexCatalogRequest.setLastmod([opCtx] {
                VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
                return vt.clusterTime().asTimestamp();
            }());
            dropIndexCatalogRequest.setName(name);
            configsvrDropIndexCatalogRequest.setUnregisterIndexCatalogRequest(
                dropIndexCatalogRequest);
            configsvrDropIndexCatalogRequest.setDbName(DatabaseName::kAdmin);
            auto commitIndexEntryResponse =
                Grid::get(opCtx)
                    ->shardRegistry()
                    ->getConfigShard()
                    ->runCommandWithFixedRetryAttempts(
                        opCtx,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        "admin",
                        CommandHelpers::appendMajorityWriteConcern(
                            configsvrDropIndexCatalogRequest.toBSON(osi.toBSON())),
                        Shard::RetryPolicy::kIdempotent);

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(commitIndexEntryResponse));

            // Ensure the index is dropped in every shard.
            ShardsvrDropIndexCatalogEntryParticipant shardsvrDropIndexCatalogEntryRequest(
                userCollectionNss);
            shardsvrDropIndexCatalogEntryRequest.setUnregisterIndexCatalogRequest(
                dropIndexCatalogRequest);
            shardsvrDropIndexCatalogEntryRequest.setDbName(DatabaseName::kAdmin);

            sharding_util::sendCommandToShards(
                opCtx,
                DatabaseName::kAdmin.db(),
                CommandHelpers::appendMajorityWriteConcern(
                    shardsvrDropIndexCatalogEntryRequest.toBSON(osi.toBSON())),
                shardIdsVec,
                executor);
        });
}
}  // namespace sharding_index_catalog_util

}  // namespace mongo
