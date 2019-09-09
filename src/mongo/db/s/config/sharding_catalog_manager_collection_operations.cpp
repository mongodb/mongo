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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <iomanip>
#include <set>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

using CollectionUUID = UUID;
using std::set;
using std::string;
using std::vector;

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));
const char kWriteConcernField[] = "writeConcern";

boost::optional<UUID> checkCollectionOptions(OperationContext* opCtx,
                                             Shard* shard,
                                             const NamespaceString& ns,
                                             const CollectionOptions options) {
    BSONObjBuilder listCollCmd;
    listCollCmd.append("listCollections", 1);
    listCollCmd.append("filter", BSON("name" << ns.coll()));

    auto response = uassertStatusOK(
        shard->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                ns.db().toString(),
                                                listCollCmd.obj(),
                                                Shard::RetryPolicy::kIdempotent));

    auto cursorObj = response.response["cursor"].Obj();
    auto collections = cursorObj["firstBatch"].Obj();
    BSONObjIterator collIter(collections);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "cannot find ns: " << ns.ns(),
            collIter.more());

    auto collectionDetails = collIter.next();
    CollectionOptions actualOptions;

    uassertStatusOK(actualOptions.parse(collectionDetails["options"].Obj()));
    // TODO: SERVER-33048 check idIndex field

    uassert(ErrorCodes::NamespaceExists,
            str::stream() << "ns: " << ns.ns()
                          << " already exists with different options: " << actualOptions.toBSON(),
            options.matchesStorageOptions(
                actualOptions, CollatorFactoryInterface::get(opCtx->getServiceContext())));

    if (actualOptions.isView()) {
        // Views don't have UUID.
        return boost::none;
    }

    auto collectionInfo = collectionDetails["info"].Obj();
    return uassertStatusOK(UUID::parse(collectionInfo["uuid"]));
}

void writeFirstChunksForShardCollection(
    OperationContext* opCtx, const InitialSplitPolicy::ShardCollectionConfig& initialChunks) {
    for (const auto& chunk : initialChunks.chunks) {
        uassertStatusOK(Grid::get(opCtx)->catalogClient()->insertConfigDocument(
            opCtx,
            ChunkType::ConfigNS,
            chunk.toConfigBSON(),
            ShardingCatalogClient::kMajorityWriteConcern));
    }
}

}  // namespace

void checkForExistingChunks(OperationContext* opCtx, const NamespaceString& nss) {
    BSONObjBuilder countBuilder;
    countBuilder.append("count", ChunkType::ConfigNS.coll());
    countBuilder.append("query", BSON(ChunkType::ns(nss.ns())));

    // OK to use limit=1, since if any chunks exist, we will fail.
    countBuilder.append("limit", 1);

    // Use readConcern local to guarantee we see any chunks that have been written and may
    // become committed; readConcern majority will not see the chunks if they have not made it
    // to the majority snapshot.
    repl::ReadConcernArgs readConcern(repl::ReadConcernLevel::kLocalReadConcern);
    readConcern.appendInfo(&countBuilder);

    auto cmdResponse = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            kConfigReadSelector,
            ChunkType::ConfigNS.db().toString(),
            countBuilder.done(),
            Shard::kDefaultConfigCommandTimeout,
            Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(cmdResponse.commandStatus);

    long long numChunks;
    uassertStatusOK(bsonExtractIntegerField(cmdResponse.response, "n", &numChunks));
    uassert(ErrorCodes::ManualInterventionRequired,
            str::stream() << "A previous attempt to shard collection " << nss.ns()
                          << " failed after writing some initial chunks to config.chunks. Please "
                             "manually delete the partially written chunks for collection "
                          << nss.ns() << " from config.chunks",
            numChunks == 0);
}

void sendDropCollectionToAllShards(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    const auto shardsStatus =
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    uassertStatusOK(shardsStatus.getStatus());

    vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    const auto dropCommandBSON = [opCtx, &nss] {
        BSONObjBuilder builder;
        builder.append("drop", nss.coll());

        if (!opCtx->getWriteConcern().usedDefault) {
            builder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
        }

        return builder.obj();
    }();

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    for (const auto& shardEntry : allShards) {
        const auto& shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

        auto swDropResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.db().toString(),
            dropCommandBSON,
            Shard::RetryPolicy::kIdempotent);

        const std::string dropCollectionErrMsg = str::stream()
            << "Error dropping collection on shard " << shardEntry.getName();

        auto dropResult = uassertStatusOKWithContext(swDropResult, dropCollectionErrMsg);
        uassertStatusOKWithContext(dropResult.writeConcernStatus, dropCollectionErrMsg);

        auto dropCommandStatus = std::move(dropResult.commandStatus);
        if (dropCommandStatus.code() == ErrorCodes::NamespaceNotFound) {
            // The dropCollection command on the shard is not idempotent, and can return
            // NamespaceNotFound. We can ignore NamespaceNotFound since we have already asserted
            // that there is no writeConcern error.
            continue;
        }

        uassertStatusOKWithContext(dropCommandStatus, dropCollectionErrMsg);
    }
}

void sendSSVAndUnsetShardingToAllShards(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    const auto shardsStatus =
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    uassertStatusOK(shardsStatus.getStatus());

    vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    for (const auto& shardEntry : allShards) {
        const auto& shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

        SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
            shardRegistry->getConfigServerConnectionString(),
            shardEntry.getName(),
            fassert(28781, ConnectionString::parse(shardEntry.getHost())),
            nss,
            ChunkVersion::DROPPED(),
            true /* isAuthoritative */,
            true /* forceRefresh */);

        auto ssvResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            ssv.toBSON(),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(ssvResult.getStatus());
        uassertStatusOK(ssvResult.getValue().commandStatus);

        auto unsetShardingStatus = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            BSON("unsetSharding" << 1),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(unsetShardingStatus);
        uassertStatusOK(unsetShardingStatus.getValue().commandStatus);
    }
}

void removeChunksAndTagsForDroppedCollection(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove chunk data
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             ChunkType::ConfigNS,
                                             BSON(ChunkType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern));

    // Remove tag data
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             TagsType::ConfigNS,
                                             BSON(TagsType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}

void ShardingCatalogManager::dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "dropCollection.start",
        nss.ns(),
        BSONObj(),
        ShardingCatalogClient::kMajorityWriteConcern));

    LOG(1) << "dropCollection " << nss.ns() << " started";

    sendDropCollectionToAllShards(opCtx, nss);

    LOG(1) << "dropCollection " << nss.ns() << " shard data deleted";

    removeChunksAndTagsForDroppedCollection(opCtx, nss);

    LOG(1) << "dropCollection " << nss.ns() << " chunk and tag data deleted";

    // Mark the collection as dropped
    CollectionType coll;
    coll.setNs(nss);
    coll.setDropped(true);
    coll.setEpoch(ChunkVersion::DROPPED().epoch());
    coll.setUpdatedAt(Grid::get(opCtx)->getNetwork()->now());

    const bool upsert = false;
    uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
        opCtx, nss, coll, upsert));

    LOG(1) << "dropCollection " << nss.ns() << " collection marked as dropped";

    sendSSVAndUnsetShardingToAllShards(opCtx, nss);

    LOG(1) << "dropCollection " << nss.ns() << " completed";

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "dropCollection", nss.ns(), BSONObj(), ShardingCatalogClient::kMajorityWriteConcern);
}

void ShardingCatalogManager::ensureDropCollectionCompleted(OperationContext* opCtx,
                                                           const NamespaceString& nss) {

    LOG(1) << "Ensuring config entries for " << nss.ns()
           << " from previous dropCollection are cleared";
    sendDropCollectionToAllShards(opCtx, nss);
    removeChunksAndTagsForDroppedCollection(opCtx, nss);
    sendSSVAndUnsetShardingToAllShards(opCtx, nss);
}

void ShardingCatalogManager::shardCollection(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const boost::optional<UUID> uuid,
                                             const ShardKeyPattern& fieldsAndOrder,
                                             const BSONObj& defaultCollation,
                                             bool unique,
                                             const vector<BSONObj>& splitPoints,
                                             bool isFromMapReduce,
                                             const ShardId& dbPrimaryShardId) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    const auto primaryShard = uassertStatusOK(shardRegistry->getShard(opCtx, dbPrimaryShardId));

    // Fail if there are partially written chunks from a previous failed shardCollection.
    checkForExistingChunks(opCtx, nss);

    // Prior to 4.0.5, zones cannot be taken into account at collection sharding time, so ignore
    // them and let the balancer apply them later
    const std::vector<TagsType> treatAsNoZonesDefined;

    // Map/reduce with output to sharded collection ignores consistency checks and requires the
    // initial chunks to be spread across shards unconditionally
    const bool treatAsEmpty = isFromMapReduce;

    // Record start in changelog
    {
        BSONObjBuilder collectionDetail;
        collectionDetail.append("shardKey", fieldsAndOrder.toBSON());
        collectionDetail.append("collection", nss.ns());
        if (uuid)
            uuid->appendToBuilder(&collectionDetail, "uuid");
        collectionDetail.append("empty", treatAsEmpty);
        collectionDetail.append("fromMapReduce", isFromMapReduce);
        collectionDetail.append("primary", primaryShard->toString());
        collectionDetail.append("numChunks", static_cast<int>(splitPoints.size() + 1));
        uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
            opCtx,
            "shardCollection.start",
            nss.ns(),
            collectionDetail.obj(),
            ShardingCatalogClient::kMajorityWriteConcern));
    }

    // Construct the collection default collator.
    std::unique_ptr<CollatorInterface> defaultCollator;
    if (!defaultCollation.isEmpty()) {
        defaultCollator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(defaultCollation));
    }

    auto optimizationType = InitialSplitPolicy::calculateOptimizationType(
        splitPoints, treatAsNoZonesDefined, treatAsEmpty);

    InitialSplitPolicy::ShardCollectionConfig initialChunks;

    if (optimizationType != InitialSplitPolicy::ShardingOptimizationType::None) {
        initialChunks =
            InitialSplitPolicy::createFirstChunksOptimized(opCtx,
                                                           nss,
                                                           fieldsAndOrder,
                                                           dbPrimaryShardId,
                                                           splitPoints,
                                                           treatAsNoZonesDefined,
                                                           optimizationType,
                                                           treatAsEmpty,
                                                           1  // numContiguousChunksPerShard
            );
    } else {
        initialChunks = InitialSplitPolicy::createFirstChunksUnoptimized(
            opCtx, nss, fieldsAndOrder, dbPrimaryShardId);
    }

    writeFirstChunksForShardCollection(opCtx, initialChunks);

    {
        CollectionType coll;
        coll.setNs(nss);
        if (uuid)
            coll.setUUID(*uuid);
        coll.setEpoch(initialChunks.collVersion().epoch());
        coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(initialChunks.collVersion().toLong()));
        coll.setKeyPattern(fieldsAndOrder.toBSON());
        coll.setDefaultCollation(defaultCollator ? defaultCollator->getSpec().toBSON() : BSONObj());
        coll.setUnique(unique);

        uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
            opCtx, nss, coll, true /*upsert*/));
    }

    auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, dbPrimaryShardId));
    invariant(!shard->isConfig());

    // Tell the primary mongod to refresh its data
    SetShardVersionRequest ssv = SetShardVersionRequest::makeForVersioningNoPersist(
        shardRegistry->getConfigServerConnectionString(),
        dbPrimaryShardId,
        primaryShard->getConnString(),
        nss,
        initialChunks.collVersion(),
        true /* isAuthoritative */,
        true /* forceRefresh */);

    auto ssvResponse =
        shard->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                "admin",
                                                ssv.toBSON(),
                                                Shard::RetryPolicy::kIdempotent);
    auto status = ssvResponse.isOK() ? std::move(ssvResponse.getValue().commandStatus)
                                     : std::move(ssvResponse.getStatus());
    if (!status.isOK()) {
        warning() << "could not update initial version of " << nss.ns() << " on shard primary "
                  << dbPrimaryShardId << causedBy(redact(status));
    }

    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        "shardCollection.end",
        nss.ns(),
        BSON("version" << initialChunks.collVersion().toString()),
        ShardingCatalogClient::kMajorityWriteConcern);
}

void ShardingCatalogManager::generateUUIDsForExistingShardedCollections(OperationContext* opCtx) {
    // Retrieve all collections in config.collections that do not have a UUID. Some collections
    // may already have a UUID if an earlier upgrade attempt failed after making some progress.
    auto shardedColls =
        uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                CollectionType::ConfigNS,
                BSON(CollectionType::uuid.name() << BSON("$exists" << false) << "dropped" << false),
                BSONObj(),   // sort
                boost::none  // limit
                ))
            .docs;

    if (shardedColls.empty()) {
        LOG(0) << "all sharded collections already have UUIDs";

        // We did a local read of the collections collection above and found that all sharded
        // collections already have UUIDs. However, the data may not be majority committed (a
        // previous setFCV attempt may have failed with a write concern error). Since the current
        // Client doesn't know the opTime of the last write to the collections collection, make it
        // wait for the last opTime in the system when we wait for writeConcern.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return;
    }

    // Generate and persist a new UUID for each collection that did not have a UUID.
    LOG(0) << "generating UUIDs for " << shardedColls.size()
           << " sharded collections that do not yet have a UUID";
    for (auto& coll : shardedColls) {
        auto collType = uassertStatusOK(CollectionType::fromBSON(coll));
        invariant(!collType.getUUID());

        auto uuid = CollectionUUID::gen();
        collType.setUUID(uuid);

        uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
            opCtx, collType.getNs(), collType, false /* upsert */));
        LOG(2) << "updated entry in config.collections for sharded collection " << collType.getNs()
               << " with generated UUID " << uuid;
    }
}

void ShardingCatalogManager::createCollection(OperationContext* opCtx,
                                              const NamespaceString& ns,
                                              const CollectionOptions& collOptions) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    auto dbEntry =
        uassertStatusOK(catalogClient->getDatabase(
                            opCtx, ns.db().toString(), repl::ReadConcernLevel::kLocalReadConcern))
            .value;
    const auto& primaryShardId = dbEntry.getPrimary();
    auto primaryShard = uassertStatusOK(shardRegistry->getShard(opCtx, primaryShardId));

    BSONObjBuilder createCmdBuilder;
    createCmdBuilder.append("create", ns.coll());
    collOptions.appendBSON(&createCmdBuilder);
    createCmdBuilder.append(kWriteConcernField, opCtx->getWriteConcern().toBSON());
    auto swResponse = primaryShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        ns.db().toString(),
        createCmdBuilder.obj(),
        Shard::RetryPolicy::kIdempotent);

    auto createStatus = Shard::CommandResponse::getEffectiveStatus(swResponse);
    if (!createStatus.isOK() && createStatus != ErrorCodes::NamespaceExists) {
        uassertStatusOK(createStatus);
    }

    checkCollectionOptions(opCtx, primaryShard.get(), ns, collOptions);

    // TODO: SERVER-33094 use UUID returned to write config.collections entries.

    // Make sure to advance the opTime if writes didn't occur during the execution of this
    // command. This is to ensure that this request will wait for the opTime that at least
    // reflects the current state (that this command observed) while waiting for replication
    // to satisfy the write concern.
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
}

}  // namespace mongo
