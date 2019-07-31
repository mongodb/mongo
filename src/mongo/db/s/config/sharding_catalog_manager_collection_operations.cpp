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
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

using CollectionUUID = UUID;
using std::set;
using std::string;
using std::vector;

MONGO_FAIL_POINT_DEFINE(writeUnshardedCollectionsToShardingCatalog);

MONGO_FAIL_POINT_DEFINE(hangCreateCollectionAfterAcquiringDistlocks);
MONGO_FAIL_POINT_DEFINE(hangCreateCollectionAfterSendingCreateToPrimaryShard);
MONGO_FAIL_POINT_DEFINE(hangCreateCollectionAfterGettingUUIDFromPrimaryShard);
MONGO_FAIL_POINT_DEFINE(hangCreateCollectionAfterWritingEntryToConfigChunks);
MONGO_FAIL_POINT_DEFINE(hangCreateCollectionAfterWritingEntryToConfigCollections);

namespace {

const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));
const char kWriteConcernField[] = "writeConcern";

const KeyPattern kUnshardedCollectionShardKey(BSON("_id" << 1));

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
    CollectionOptions actualOptions =
        uassertStatusOK(CollectionOptions::parse(collectionDetails["options"].Obj()));
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

void writeFirstChunksForCollection(OperationContext* opCtx,
                                   const InitialSplitPolicy::ShardCollectionConfig& initialChunks) {
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

Status ShardingCatalogManager::dropCollection(OperationContext* opCtx, const NamespaceString& nss) {
    const Status logStatus =
        ShardingLogging::get(opCtx)->logChangeChecked(opCtx,
                                                      "dropCollection.start",
                                                      nss.ns(),
                                                      BSONObj(),
                                                      ShardingCatalogClient::kMajorityWriteConcern);
    if (!logStatus.isOK()) {
        return logStatus;
    }

    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    const auto shardsStatus =
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    if (!shardsStatus.isOK()) {
        return shardsStatus.getStatus();
    }
    vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    LOG(1) << "dropCollection " << nss.ns() << " started";

    const auto dropCommandBSON = [opCtx, &nss] {
        BSONObjBuilder builder;
        builder.append("drop", nss.coll());

        if (!opCtx->getWriteConcern().usedDefault) {
            builder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
        }

        return builder.obj();
    }();

    std::map<std::string, BSONObj> errors;
    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    for (const auto& shardEntry : allShards) {
        auto swShard = shardRegistry->getShard(opCtx, shardEntry.getName());
        if (!swShard.isOK()) {
            return swShard.getStatus();
        }

        const auto& shard = swShard.getValue();

        auto swDropResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.db().toString(),
            dropCommandBSON,
            Shard::RetryPolicy::kIdempotent);

        if (!swDropResult.isOK()) {
            return swDropResult.getStatus().withContext(
                str::stream() << "Error dropping collection on shard " << shardEntry.getName());
        }

        auto& dropResult = swDropResult.getValue();

        auto dropStatus = std::move(dropResult.commandStatus);
        auto wcStatus = std::move(dropResult.writeConcernStatus);
        if (!dropStatus.isOK() || !wcStatus.isOK()) {
            if (dropStatus.code() == ErrorCodes::NamespaceNotFound && wcStatus.isOK()) {
                // Generally getting NamespaceNotFound is okay to ignore as it simply means that
                // the collection has already been dropped or doesn't exist on this shard.
                // If, however, we get NamespaceNotFound but also have a write concern error then we
                // can't confirm whether the fact that the namespace doesn't exist is actually
                // committed.  Thus we must still fail on NamespaceNotFound if there is also a write
                // concern error. This can happen if we call drop, it succeeds but with a write
                // concern error, then we retry the drop.
                continue;
            }

            errors.emplace(shardEntry.getHost(), std::move(dropResult.response));
        }
    }

    if (!errors.empty()) {
        StringBuilder sb;
        sb << "Dropping collection failed on the following hosts: ";

        for (auto it = errors.cbegin(); it != errors.cend(); ++it) {
            if (it != errors.cbegin()) {
                sb << ", ";
            }

            sb << it->first << ": " << it->second;
        }

        return {ErrorCodes::OperationFailed, sb.str()};
    }

    LOG(1) << "dropCollection " << nss.ns() << " shard data deleted";

    // Remove chunk data
    Status result =
        catalogClient->removeConfigDocuments(opCtx,
                                             ChunkType::ConfigNS,
                                             BSON(ChunkType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << nss.ns() << " chunk data deleted";

    // Remove tag data
    result = catalogClient->removeConfigDocuments(opCtx,
                                                  TagsType::ConfigNS,
                                                  BSON(TagsType::ns(nss.ns())),
                                                  ShardingCatalogClient::kMajorityWriteConcern);

    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << nss.ns() << " tag data deleted";

    // Mark the collection as dropped
    CollectionType coll;
    coll.setNs(nss);
    coll.setDropped(true);
    coll.setEpoch(ChunkVersion::DROPPED().epoch());
    coll.setUpdatedAt(Grid::get(opCtx)->getNetwork()->now());

    const bool upsert = false;
    result = ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
        opCtx, nss, coll, upsert);
    if (!result.isOK()) {
        return result;
    }

    LOG(1) << "dropCollection " << nss.ns() << " collection marked as dropped";

    for (const auto& shardEntry : allShards) {
        auto swShard = shardRegistry->getShard(opCtx, shardEntry.getName());
        if (!swShard.isOK()) {
            return swShard.getStatus();
        }

        const auto& shard = swShard.getValue();

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

        if (!ssvResult.isOK()) {
            return ssvResult.getStatus();
        }

        auto ssvStatus = std::move(ssvResult.getValue().commandStatus);
        if (!ssvStatus.isOK()) {
            return ssvStatus;
        }

        auto unsetShardingStatus = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            BSON("unsetSharding" << 1),
            Shard::RetryPolicy::kIdempotent);

        if (!unsetShardingStatus.isOK()) {
            return unsetShardingStatus.getStatus();
        }

        auto unsetShardingResult = std::move(unsetShardingStatus.getValue().commandStatus);
        if (!unsetShardingResult.isOK()) {
            return unsetShardingResult;
        }
    }

    LOG(1) << "dropCollection " << nss.ns() << " completed";

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "dropCollection", nss.ns(), BSONObj(), ShardingCatalogClient::kMajorityWriteConcern);

    return Status::OK();
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

    writeFirstChunksForCollection(opCtx, initialChunks);

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
    if (MONGO_FAIL_POINT(hangCreateCollectionAfterAcquiringDistlocks)) {
        log() << "Hit hangCreateCollectionAfterAcquiringDistlocks";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
            opCtx, hangCreateCollectionAfterAcquiringDistlocks);
    }

    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Forward the create to the primary shard to either create the collection or verify that the
    // collection already exists with the same options.

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

    if (MONGO_FAIL_POINT(hangCreateCollectionAfterSendingCreateToPrimaryShard)) {
        log() << "Hit hangCreateCollectionAfterSendingCreateToPrimaryShard";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
            opCtx, hangCreateCollectionAfterSendingCreateToPrimaryShard);
    }

    auto createStatus = Shard::CommandResponse::getEffectiveStatus(swResponse);
    if (!createStatus.isOK() && createStatus != ErrorCodes::NamespaceExists) {
        uassertStatusOK(createStatus);
    }

    const auto uuid = checkCollectionOptions(opCtx, primaryShard.get(), ns, collOptions);

    if (MONGO_FAIL_POINT(hangCreateCollectionAfterGettingUUIDFromPrimaryShard)) {
        log() << "Hit hangCreateCollectionAfterGettingUUIDFromPrimaryShard";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
            opCtx, hangCreateCollectionAfterGettingUUIDFromPrimaryShard);
    }

    if (collOptions.isView()) {
        // Views are not written to the sharding catalog.
        return;
    }

    uassert(51248,
            str::stream() << "Expected to get back UUID from primary shard for new collection "
                          << ns.ns(),
            uuid);

    // Insert the collection into the sharding catalog if it does not already exist.

    const auto swExistingCollType =
        catalogClient->getCollection(opCtx, ns, repl::ReadConcernLevel::kLocalReadConcern);
    if (swExistingCollType != ErrorCodes::NamespaceNotFound) {
        const auto existingCollType = uassertStatusOK(swExistingCollType).value;
        LOG(0) << "Collection " << ns.ns() << " already exists in sharding catalog as "
               << existingCollType.toBSON() << ", createCollection not writing new entry";
        return;
    }

    InitialSplitPolicy::ShardCollectionConfig initialChunks;
    ChunkVersion version(1, 0, OID::gen());
    initialChunks.chunks.emplace_back(ns,
                                      ChunkRange(kUnshardedCollectionShardKey.globalMin(),
                                                 kUnshardedCollectionShardKey.globalMax()),
                                      version,
                                      primaryShardId);

    auto& chunk = initialChunks.chunks.back();
    const Timestamp validAfter = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();
    chunk.setHistory({ChunkHistory(std::move(validAfter), primaryShardId)});

    // Construct the collection default collator.
    std::unique_ptr<CollatorInterface> defaultCollator;
    if (!collOptions.collation.isEmpty()) {
        defaultCollator = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                              ->makeFromBSON(collOptions.collation));
    }

    CollectionType targetCollType;
    targetCollType.setNs(ns);
    targetCollType.setDefaultCollation(defaultCollator ? defaultCollator->getSpec().toBSON()
                                                       : BSONObj());
    targetCollType.setUUID(*uuid);
    targetCollType.setEpoch(initialChunks.collVersion().epoch());
    targetCollType.setUpdatedAt(Date_t::fromMillisSinceEpoch(initialChunks.collVersion().toLong()));
    targetCollType.setKeyPattern(kUnshardedCollectionShardKey.toBSON());
    targetCollType.setUnique(false);
    targetCollType.setDistributionMode(CollectionType::DistributionMode::kUnsharded);
    uassertStatusOK(targetCollType.validate());

    try {
        checkForExistingChunks(opCtx, ns);
    } catch (const ExceptionFor<ErrorCodes::ManualInterventionRequired>&) {
        LOG(0) << "Found orphaned chunk metadata for " << ns.ns()
               << ", going to remove it before writing new chunk metadata for createCollection";
        uassertStatusOK(
            catalogClient->removeConfigDocuments(opCtx,
                                                 ChunkType::ConfigNS,
                                                 BSON(ChunkType::ns(ns.ns())),
                                                 ShardingCatalogClient::kLocalWriteConcern));
    }

    if (MONGO_FAIL_POINT(writeUnshardedCollectionsToShardingCatalog)) {
        LOG(0) << "Going to write initial chunk for new unsharded collection " << ns.ns() << ": "
               << chunk.toString();
        writeFirstChunksForCollection(opCtx, initialChunks);

        if (MONGO_FAIL_POINT(hangCreateCollectionAfterWritingEntryToConfigChunks)) {
            log() << "Hit hangCreateCollectionAfterWritingEntryToConfigChunks";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
                opCtx, hangCreateCollectionAfterWritingEntryToConfigChunks);
        }

        LOG(0) << "Going to write collection entry for new unsharded collection " << ns.ns() << ": "
               << targetCollType.toBSON();
        uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
            opCtx, ns, targetCollType, true /*upsert*/));

        if (MONGO_FAIL_POINT(hangCreateCollectionAfterWritingEntryToConfigCollections)) {
            log() << "Hit hangCreateCollectionAfterWritingEntryToConfigCollections";
            MONGO_FAIL_POINT_PAUSE_WHILE_SET_OR_INTERRUPTED(
                opCtx, hangCreateCollectionAfterWritingEntryToConfigCollections);
        }
    }
}

void ShardingCatalogManager::refineCollectionShardKey(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const ShardKeyPattern& newShardKeyPattern) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations. Take _kZoneOpLock in exclusive mode to prevent concurrent zone operations.
    // TODO(SERVER-25359): Replace with a collection-specific lock map to allow splits/merges/
    // move chunks on different collections to proceed in parallel.
    Lock::ExclusiveLock chunkLk(opCtx->lockState(), _kChunkOpLock);
    Lock::ExclusiveLock zoneLk(opCtx->lockState(), _kZoneOpLock);

    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    const auto newEpoch = OID::gen();

    auto collType = uassertStatusOK(catalogClient->getCollection(opCtx, nss)).value;
    const auto oldShardKeyPattern = ShardKeyPattern(collType.getKeyPattern());
    collType.setEpoch(newEpoch);
    collType.setKeyPattern(newShardKeyPattern.getKeyPattern());

    uassertStatusOK(ShardingCatalogClientImpl::updateShardingCatalogEntryForCollection(
        opCtx, nss, collType, false /* upsert */));

    const auto oldFields = oldShardKeyPattern.toBSON();
    const auto newFields =
        newShardKeyPattern.toBSON().filterFieldsUndotted(oldFields, false /* inFilter */);

    // Construct query objects for calls to 'updateConfigDocument(s)' below.
    BSONObjBuilder notGlobalMaxBuilder, isGlobalMaxBuilder;
    notGlobalMaxBuilder.append(ChunkType::ns.name(), nss.ns());
    isGlobalMaxBuilder.append(ChunkType::ns.name(), nss.ns());
    for (const auto& fieldElem : oldFields) {
        notGlobalMaxBuilder.append("max." + fieldElem.fieldNameStringData(), BSON("$ne" << MAXKEY));
        isGlobalMaxBuilder.append("max." + fieldElem.fieldNameStringData(), BSON("$eq" << MAXKEY));
    }
    const auto notGlobalMaxQuery = notGlobalMaxBuilder.obj();
    const auto isGlobalMaxQuery = isGlobalMaxBuilder.obj();

    // The defaultBounds object sets the bounds of each new field in the refined key to MinKey. The
    // globalMaxBounds object corrects the max bounds of the global max chunk/tag to MaxKey.
    //
    // Example: oldKeyDoc = {a: 1}
    //          newKeyDoc = {a: 1, b: 1, c: 1}
    //          defaultBounds = {min.b: MinKey, min.c: MinKey, max.b: MinKey, max.c: MinKey}
    //          globalMaxBounds = {min.b: MinKey, min.c: MinKey, max.b: MaxKey, max.c: MaxKey}
    BSONObjBuilder defaultBoundsBuilder, globalMaxBoundsBuilder;
    for (const auto& fieldElem : newFields) {
        defaultBoundsBuilder.appendMinKey("min." + fieldElem.fieldNameStringData());
        defaultBoundsBuilder.appendMinKey("max." + fieldElem.fieldNameStringData());

        globalMaxBoundsBuilder.appendMinKey("min." + fieldElem.fieldNameStringData());
        globalMaxBoundsBuilder.appendMaxKey("max." + fieldElem.fieldNameStringData());
    }
    const auto defaultBounds = defaultBoundsBuilder.obj();
    const auto globalMaxBounds = globalMaxBoundsBuilder.obj();

    // Update all config.chunks entries for the given namespace by setting (i) their epoch to the
    // newly-generated objectid, (ii) their bounds for each new field in the refined key to MinKey
    // (except for the global max chunk where the max bounds are set to MaxKey), and unsetting (iii)
    // their jumbo field.
    uassertStatusOK(catalogClient->updateConfigDocuments(
        opCtx,
        ChunkType::ConfigNS,
        notGlobalMaxQuery,
        BSON("$set" << BSON(ChunkType::epoch(newEpoch)) << "$max" << defaultBounds << "$unset"
                    << BSON(ChunkType::jumbo(true))),
        false,  // upsert
        ShardingCatalogClient::kLocalWriteConcern));

    uassertStatusOK(catalogClient->updateConfigDocument(
        opCtx,
        ChunkType::ConfigNS,
        isGlobalMaxQuery,
        BSON("$set" << BSON(ChunkType::epoch(newEpoch)) << "$max" << globalMaxBounds << "$unset"
                    << BSON(ChunkType::jumbo(true))),
        false,  // upsert
        ShardingCatalogClient::kLocalWriteConcern));

    // Update all config.tags entries for the given namespace by setting their bounds for each new
    // field in the refined key to MinKey (except for the global max tag where the max bounds are
    // set to MaxKey).
    uassertStatusOK(
        catalogClient->updateConfigDocuments(opCtx,
                                             TagsType::ConfigNS,
                                             notGlobalMaxQuery,
                                             BSON("$max" << defaultBounds),
                                             false,  // upsert
                                             ShardingCatalogClient::kLocalWriteConcern));

    uassertStatusOK(catalogClient->updateConfigDocument(opCtx,
                                                        TagsType::ConfigNS,
                                                        isGlobalMaxQuery,
                                                        BSON("$max" << globalMaxBounds),
                                                        false,  // upsert
                                                        ShardingCatalogClient::kLocalWriteConcern));
}

}  // namespace mongo
