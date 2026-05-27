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

#include "mongo/db/shard_role/shard_catalog/shard_catalog_history_cleanup.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/server_parameters_gen.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::shard_catalog_helper {

namespace {

void cleanupCollectionEntry(OperationContext* opCtx, const NamespaceString& nss) {
    auto acquisition = acquireCollectionOrView(
        opCtx,
        CollectionOrViewAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);

    if (!acquisition.collectionExists()) {
        // Collection got dropped, the drop coordinator will remove all the metadata on this shard.
        LOGV2_INFO(12620107,
                   "Skipping shard catalog cleanup as it got dropped and there shouldn't be any "
                   "data present",
                   "collection"_attr = nss);
        return;
    }

    const auto scopedDsr = DatabaseShardingRuntime::acquireShared(opCtx, nss.dbName());
    const auto isDatabaseCriticalSectionActive =
        scopedDsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite).has_value();
    if (isDatabaseCriticalSectionActive) {
        LOGV2(12764901,
              "Skipping shard catalog cleanup as the database critical section is active",
              "collection"_attr = nss);
        return;
    }

    const auto isDbPrimary =
        scopedDsr->getDbPrimaryShard(opCtx) == ShardingState::get(opCtx)->shardId();
    if (isDbPrimary) {
        // The primary must always contain the collection entry in order to avoid having
        // tracked/untracked issues.
        LOGV2(12764903,
              "Skipping shard catalog cleanup as this shard is the dbPrimary",
              "collection"_attr = nss);
        return;
    }

    const auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, nss);
    const auto isCriticalSectionActive =
        scopedCsr->getCriticalSectionSignal(ShardingMigrationCriticalSection::kWrite).has_value();
    if (isCriticalSectionActive) {
        LOGV2(12620106,
              "Skipping shard catalog cleanup as critical section is active",
              "collection"_attr = nss);
        // Do an early return since the critical section is active, meaning changes are being
        // made to the collection metadata. Bail out to prevent conflicts.
        return;
    }

    DBDirectClient client{opCtx};
    auto chunkCount =
        client.count(NamespaceString::kConfigShardCatalogChunksNamespace,
                     BSON(ChunkType::collectionUUID() << acquisition.getCollection().uuid()));
    if (chunkCount != 0) {
        LOGV2_INFO(12620104,
                   "Skipping shard catalog cleanup as it still holds chunks",
                   "collection"_attr = nss);
        // Collection still has chunks in the shard.
        return;
    }

    LOGV2_INFO(12764900,
               "Cleaning up shard catalog entries as it is no longer visible in this shard",
               "collection"_attr = nss);
    auto serializedNs = NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault());
    const auto commandReply =
        client.removeAcknowledged(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                                  BSON(CollectionType::kNssFieldName << serializedNs));
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));
}

std::vector<NamespaceString> getStaleCollectionEntries(OperationContext* opCtx) {
    std::vector<NamespaceString> collections;

    static constexpr auto kChunksFieldName = "collection_chunks"_sd;

    AggregateCommandRequest aggRequest{NamespaceString::kConfigShardCatalogCollectionsNamespace};
    // Lookup collections that do not have any chunks on the durable catalog.
    aggRequest.setPipeline(
        {BSON("$lookup" << BSON("from" << NamespaceString::kConfigShardCatalogChunksNamespace.coll()
                                       << "localField" << CollectionType::kUuidFieldName
                                       << "foreignField" << ChunkType::collectionUUID() << "as"
                                       << kChunksFieldName << "pipeline"
                                       << BSON_ARRAY(BSON("$limit" << 1)))),
         BSON("$match" << BSON(kChunksFieldName << BSONArray())),
         BSON("$project" << BSON(CollectionType::kNssFieldName << 1))});

    DBDirectClient client(opCtx);
    auto cursor = uassertStatusOKWithContext(
        DBClientCursor::fromAggregationRequest(
            &client, aggRequest, false /* secondaryOk */, true /* useExhaust */),
        "Failed to establish a cursor for aggregation");
    while (cursor->more()) {
        const auto obj = cursor->nextSafe();
        auto nssStringData = obj.getField(CollectionType::kNssFieldName).valueStringData();
        auto nss = NamespaceStringUtil::deserialize(
            boost::none, nssStringData, SerializationContext::stateDefault());
        collections.emplace_back(std::move(nss));
    }
    return collections;
}
}  // namespace

StorageEngine::TimestampMonitor::TimestampListener kShardCatalogHistoryCleanupTimestampListener(
    [](OperationContext* opCtx, const StorageEngine::TimestampMonitor::Timestamps& timestamp) {
        if (!gEnableBackgroundCleanupOfShardCatalog.loadRelaxed()) {
            LOGV2_DEBUG(12620105, 1, "Skipping cleanup of shard catalog");
            return;
        }

        auto oldest = timestamp.oldest;
        auto* service = opCtx->getServiceContext();
        auto const shardingState = ShardingState::get(service);
        if (!shardingState->enabled()) {
            return;
        }

        // Optimistic check that we run the cleanup only on primary.
        // Note: It still can fail during the cleanup, but we will handle that error
        if (repl::ReplicationCoordinator::get(opCtx) == nullptr ||
            !repl::ReplicationCoordinator::get(opCtx)->getMemberState().primary()) {
            return;
        }

        // We fix the FCV region here in order to ensure cleanup only works while FCV is fully 9.0
        FixedFCVRegion guard{opCtx};
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        if (!(feature_flags::gAuthoritativeShardsDDL.isEnabled(VersionContext::getDecoration(opCtx),
                                                               fcvSnapshot) &&
              feature_flags::gAuthoritativeShardsCRUD.isEnabled(
                  VersionContext::getDecoration(opCtx), fcvSnapshot))) {
            // The shard is not yet doing authoritative DDLs, as such it can skip the cleanup.
            return;
        }

        auto shardId = shardingState->shardId();

        PersistentTaskStore<ChunkType> chunkStore{
            NamespaceString::kConfigShardCatalogChunksNamespace};
        try {
            LOGV2_DEBUG(12620103,
                        1,
                        "Performing cleanup of stale chunks and collection entries",
                        "oldestTimestamp"_attr = oldest);
            chunkStore.remove(opCtx,
                              BSON(ChunkType::shard()
                                   << BSON("$ne" << shardId.toString())
                                   << ChunkType::onCurrentShardSince() << BSON("$lt" << oldest)));

            const auto collectionsToRemove = getStaleCollectionEntries(opCtx);
            for (const auto& collNss : collectionsToRemove) {
                LOGV2(12620101,
                      "Cleaning up potentially stale collection entry from durable shard catalog",
                      "collection"_attr = collNss);
                cleanupCollectionEntry(opCtx, collNss);
            }
        } catch (const ExceptionFor<ErrorCodes::FailedToSatisfyReadPreference>&) {
            // Primary can be killed in the middle of the removal.
            return;
        } catch (const ExceptionFor<ErrorCodes::WriteConcernTimeout>&) {
            // Best-effort cleanup; retry on next pass.
            return;
        } catch (const ExceptionFor<ErrorCodes::InternalError>& ex) {
            // Ignore failAllRemoves failpoint
            if (ex.reason().find("failAllRemoves") != std::string::npos) {
                return;
            }
            // Otherwise, re-throw the internal error
            throw;
        } catch (const DBException& exception) {
            auto status = exception.toStatus();
            // Stepdown / primary change mid-removal; next oldest-timestamp pass will retry.
            if (ErrorCodes::isNotPrimaryError(status.code())) {
                return;
            }
            // Otherwise, re-throw the DBException
            throw;
        }
    });
}  // namespace mongo::shard_catalog_helper
