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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include <iomanip>
#include <set>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/request_types/set_shard_version_request.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

using CollectionUUID = UUID;
using std::set;
using std::string;
using std::vector;

MONGO_FAIL_POINT_DEFINE(hangRefineCollectionShardKeyBeforeUpdatingChunks);
MONGO_FAIL_POINT_DEFINE(hangRefineCollectionShardKeyBeforeCommit);

namespace {
const ReadPreferenceSetting kConfigReadSelector(ReadPreference::Nearest, TagSet{});
static constexpr int kMaxNumStaleShardVersionRetries = 10;
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

void triggerFireAndForgetShardRefreshes(OperationContext* opCtx, const CollectionType& coll) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto allShards = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getAllShards(
                                               opCtx, repl::ReadConcernLevel::kLocalReadConcern))
                               .value;

    for (const auto& shardEntry : allShards) {
        const auto query = [&]() {
            if (coll.getTimestamp()) {
                return BSON(ChunkType::collectionUUID << coll.getUuid()
                                                      << ChunkType::shard(shardEntry.getName()));
            } else {
                return BSON(ChunkType::ns(coll.getNss().ns())
                            << ChunkType::shard(shardEntry.getName()));
            }
        }();

        const auto chunk = uassertStatusOK(shardRegistry->getConfigShard()->exhaustiveFindOnConfig(
                                               opCtx,
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               repl::ReadConcernLevel::kLocalReadConcern,
                                               ChunkType::ConfigNS,
                                               query,
                                               BSONObj(),
                                               1LL))
                               .docs;

        invariant(chunk.size() == 0 || chunk.size() == 1);

        if (chunk.size() == 1) {
            const auto shard =
                uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

            // This is a best-effort attempt to refresh the shard 'shardEntry'. Fire and forget an
            // asynchronous '_flushRoutingTableCacheUpdates' request.
            shard->runFireAndForgetCommand(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                NamespaceString::kAdminDb.toString(),
                BSON(_flushRoutingTableCacheUpdates::kCommandName << coll.getNss().ns()));
        }
    }
}

}  // namespace

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

        ChunkVersion::IGNORED().appendToCommand(&builder);
        return builder.obj();
    }();

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    for (const auto& shardEntry : allShards) {
        bool keepTrying;
        size_t numStaleShardVersionAttempts = 0;
        do {
            const auto& shard =
                uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

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
                keepTrying = false;
            } else if (ErrorCodes::isStaleShardVersionError(dropCommandStatus.code())) {
                numStaleShardVersionAttempts++;
                if (numStaleShardVersionAttempts == kMaxNumStaleShardVersionRetries) {
                    uassertStatusOKWithContext(dropCommandStatus,
                                               str::stream() << dropCollectionErrMsg
                                                             << " due to exceeded retry attempts");
                }
                // No need to refresh cache, the command was sent with ChunkVersion::IGNORED and the
                // shard is allowed to throw, which means that the drop will serialize behind a
                // refresh.
                keepTrying = true;
            } else {
                uassertStatusOKWithContext(dropCommandStatus, dropCollectionErrMsg);
                keepTrying = false;
            }
        } while (keepTrying);
    }
}

void sendSSVToAllShards(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    const auto shardsStatus =
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    uassertStatusOK(shardsStatus.getStatus());

    vector<ShardType> allShards = std::move(shardsStatus.getValue().value);

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    for (const auto& shardEntry : allShards) {
        const auto& shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

        SetShardVersionRequest ssv(
            nss, ChunkVersion::UNSHARDED(), true /* isAuthoritative */, true /* forceRefresh */);

        auto ssvResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            ssv.toBSON(),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(ssvResult.getStatus());
        uassertStatusOK(ssvResult.getValue().commandStatus);
    }
}

void removeChunksForDroppedCollection(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nssOrUUID) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove chunk data
    const auto chunksQuery = [&]() {
        if (nssOrUUID.uuid()) {
            return BSON(ChunkType::collectionUUID << *nssOrUUID.uuid());
        } else {
            return BSON(ChunkType::ns(nssOrUUID.nss()->ns()));
        }
    }();
    uassertStatusOK(catalogClient->removeConfigDocuments(
        opCtx, ChunkType::ConfigNS, chunksQuery, ShardingCatalogClient::kMajorityWriteConcern));
}

void removeTagsForDroppedCollection(OperationContext* opCtx, const NamespaceString& nss) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

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

    LOGV2_DEBUG(21924,
                1,
                "dropCollection {namespace} started",
                "dropCollection started",
                "namespace"_attr = nss.ns());

    sendDropCollectionToAllShards(opCtx, nss);

    LOGV2_DEBUG(21925,
                1,
                "dropCollection {namespace} shard data deleted",
                "dropCollection shard data deleted",
                "namespace"_attr = nss.ns());

    try {
        const auto catalogClient = Grid::get(opCtx)->catalogClient();
        auto collType = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
        const auto nssOrUUID = [&]() {
            if (collType.getTimestamp()) {
                return NamespaceStringOrUUID(std::string(), collType.getUuid());
            } else {
                return NamespaceStringOrUUID(collType.getNss());
            }
        }();
        removeChunksForDroppedCollection(opCtx, nssOrUUID);
        removeTagsForDroppedCollection(opCtx, nss);

        LOGV2_DEBUG(21926,
                    1,
                    "dropCollection {namespace} chunk and tag data deleted",
                    "dropCollection chunk and tag data deleted",
                    "namespace"_attr = nss.ns());

        uassertStatusOK(
            catalogClient->removeConfigDocuments(opCtx,
                                                 CollectionType::ConfigNS,
                                                 BSON(CollectionType::kNssFieldName << nss.ns()),
                                                 ShardingCatalogClient::kMajorityWriteConcern));
        LOGV2_DEBUG(21927,
                    1,
                    "dropCollection {namespace} collection entry deleted",
                    "dropCollection collection entry deleted",
                    "namespace"_attr = nss.ns());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        LOGV2(5310500,
              "dropCollection {namespace} collection entry not found",
              "dropCollection {namespace} collection entry not found",
              "namespace"_attr = nss.ns());
    }

    sendSSVToAllShards(opCtx, nss);

    LOGV2_DEBUG(21928,
                1,
                "dropCollection {namespace} completed",
                "dropCollection completed",
                "namespace"_attr = nss.ns());

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "dropCollection", nss.ns(), BSONObj(), ShardingCatalogClient::kMajorityWriteConcern);
}

void ShardingCatalogManager::ensureDropCollectionCompleted(OperationContext* opCtx,
                                                           const NamespaceString& nss) {

    LOGV2_DEBUG(21929,
                1,
                "Ensuring config entries for {namespace} from previous dropCollection are cleared",
                "Ensuring config entries from previous dropCollection are cleared",
                "namespace"_attr = nss.ns());
    sendDropCollectionToAllShards(opCtx, nss);

    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    removeTagsForDroppedCollection(opCtx, nss);
    sendSSVToAllShards(opCtx, nss);
}

// Returns the pipeline updates to be used for updating a refined collection's chunk and tag
// documents.
//
// The chunk updates:
// [{$set: {
//    lastmodEpoch: <new epoch>,
//    min: {$arrayToObject: {$concatArrays: [
//      {$objectToArray: "$min"},
//      {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//    ]}},
//    max: {$let: {
//      vars: {maxAsArray: {$objectToArray: "$max"}},
//      in: {
//        {$arrayToObject: {$concatArrays: [
//          "$$maxAsArray",
//          {$cond: {
//            if: {$allElementsTrue: [{$map: {
//              input: "$$maxAsArray",
//              in: {$eq: [{$type: "$$this.v"}, "maxKey"]},
//            }}]},
//            then: {$literal: [{k: <new_sk_suffix_1>, v: MaxKey}, ...]},
//            else: {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//          }}
//        ]}}
//      }
//    }}
//  }},
//  {$unset: "jumbo"}]
//
// The tag update:
// [{$set: {
//    min: {$arrayToObject: {$concatArrays: [
//      {$objectToArray: "$min"},
//      {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//    ]}},
//    max: {$let: {
//      vars: {maxAsArray: {$objectToArray: "$max"}},
//      in: {
//        {$arrayToObject: {$concatArrays: [
//          "$$maxAsArray",
//          {$cond: {
//            if: {$allElementsTrue: [{$map: {
//              input: "$$maxAsArray",
//              in: {$eq: [{$type: "$$this.v"}, "maxKey"]},
//            }}]},
//            then: {$literal: [{k: <new_sk_suffix_1>, v: MaxKey}, ...]},
//            else: {$literal: [{k: <new_sk_suffix_1>, v: MinKey}, ...]},
//          }}
//        ]}}
//      }
//    }}
//  }}]
std::pair<std::vector<BSONObj>, std::vector<BSONObj>> makeChunkAndTagUpdatesForRefine(
    const BSONObj& newShardKeyFields,
    OID newEpoch,
    const boost::optional<Timestamp>& newTimestamp) {
    // Make the $literal objects used in the $set below to add new fields to the boundaries of the
    // existing chunks and tags that may include "." characters.
    //
    // Example: oldKeyDoc = {a: 1}
    //          newKeyDoc = {a: 1, b: 1, "c.d": 1}
    //          literalMinObject = {$literal: [{k: "b", v: MinKey}, {k: "c.d", v: MinKey}]}
    //          literalMaxObject = {$literal: [{k: "b", v: MaxKey}, {k: "c.d", v: MaxKey}]}
    BSONArrayBuilder literalMinObjectBuilder, literalMaxObjectBuilder;
    for (const auto& fieldElem : newShardKeyFields) {
        literalMinObjectBuilder.append(
            BSON("k" << fieldElem.fieldNameStringData() << "v" << MINKEY));
        literalMaxObjectBuilder.append(
            BSON("k" << fieldElem.fieldNameStringData() << "v" << MAXKEY));
    }
    auto literalMinObject = BSON("$literal" << literalMinObjectBuilder.arr());
    auto literalMaxObject = BSON("$literal" << literalMaxObjectBuilder.arr());

    // Both the chunks and tags updates share the base of this $set modifier.
    auto extendMinAndMaxModifier = BSON(
        "min"
        << BSON("$arrayToObject" << BSON("$concatArrays" << BSON_ARRAY(BSON("$objectToArray"
                                                                            << "$min")
                                                                       << literalMinObject)))
        << "max"
        << BSON("$let" << BSON(
                    "vars"
                    << BSON("maxAsArray" << BSON("$objectToArray"
                                                 << "$max"))
                    << "in"
                    << BSON("$arrayToObject" << BSON(
                                "$concatArrays" << BSON_ARRAY(
                                    "$$maxAsArray"
                                    << BSON("$cond" << BSON(
                                                "if" << BSON("$allElementsTrue" << BSON_ARRAY(BSON(
                                                                 "$map" << BSON(
                                                                     "input"
                                                                     << "$$maxAsArray"
                                                                     << "in"
                                                                     << BSON("$eq" << BSON_ARRAY(
                                                                                 BSON("$type"
                                                                                      << "$$this.v")
                                                                                 << "maxKey"))))))
                                                     << "then" << literalMaxObject << "else"
                                                     << literalMinObject))))))));

    // The chunk updates change the min and max fields, and additionally set the new epoch and the
    // new timestamp and unset the jumbo field.
    std::vector<BSONObj> chunkUpdates;
    chunkUpdates.emplace_back(
        BSON("$set" << extendMinAndMaxModifier.addFields(BSON(ChunkType::epoch(newEpoch)))));

    if (newTimestamp) {
        chunkUpdates.emplace_back(BSON("$set" << extendMinAndMaxModifier.addFields(
                                           BSON(ChunkType::timestamp(*newTimestamp)))));
    }
    chunkUpdates.emplace_back(BSON("$unset" << ChunkType::jumbo()));

    // The tag updates only change the min and max fields.
    std::vector<BSONObj> tagUpdates;
    tagUpdates.emplace_back(BSON("$set" << extendMinAndMaxModifier.getOwned()));

    return std::make_pair(std::move(chunkUpdates), std::move(tagUpdates));
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

    Timer executionTimer, totalTimer;
    const auto newEpoch = OID::gen();

    auto collType = Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
    const auto oldShardKeyPattern = ShardKeyPattern(collType.getKeyPattern());

    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "refineCollectionShardKey.start",
        nss.ns(),
        BSON("oldKey" << oldShardKeyPattern.toBSON() << "newKey" << newShardKeyPattern.toBSON()
                      << "oldEpoch" << collType.getEpoch() << "newEpoch" << newEpoch),
        ShardingCatalogClient::kLocalWriteConcern));

    const auto oldFields = oldShardKeyPattern.toBSON();
    const auto newFields =
        newShardKeyPattern.toBSON().filterFieldsUndotted(oldFields, false /* inFilter */);

    collType.setEpoch(newEpoch);
    collType.setKeyPattern(newShardKeyPattern.getKeyPattern());

    boost::optional<Timestamp> newTimestamp;
    if (collType.getTimestamp()) {
        auto now = VectorClock::get(opCtx)->getTime();
        newTimestamp = now.clusterTime().asTimestamp();
        collType.setTimestamp(newTimestamp);
    }

    auto updateCollectionAndChunksFn = [&](OperationContext* opCtx, TxnNumber txnNumber) {
        // Update the config.collections entry for the given namespace.
        updateShardingCatalogEntryForCollectionInTxn(
            opCtx, nss, collType, false /* upsert */, txnNumber);

        LOGV2(21933,
              "refineCollectionShardKey updated collection entry for {namespace}: took "
              "{durationMillis} ms. Total time taken: {totalTimeMillis} ms.",
              "refineCollectionShardKey updated collection entry",
              "namespace"_attr = nss.ns(),
              "durationMillis"_attr = executionTimer.millis(),
              "totalTimeMillis"_attr = totalTimer.millis());
        executionTimer.reset();

        if (MONGO_unlikely(hangRefineCollectionShardKeyBeforeUpdatingChunks.shouldFail())) {
            LOGV2(21934, "Hit hangRefineCollectionShardKeyBeforeUpdatingChunks failpoint");
            hangRefineCollectionShardKeyBeforeUpdatingChunks.pauseWhileSet(opCtx);
        }

        auto [chunkUpdates, tagUpdates] =
            makeChunkAndTagUpdatesForRefine(newFields, newEpoch, newTimestamp);

        // Update all config.chunks entries for the given namespace by setting (i) their epoch
        // to the newly-generated objectid, (ii) their bounds for each new field in the refined
        // key to MinKey (except for the global max chunk where the max bounds are set to
        // MaxKey), and unsetting (iii) their jumbo field.
        const auto chunksQuery = [&]() {
            if (collType.getTimestamp()) {
                return BSON(ChunkType::collectionUUID << collType.getUuid());
            } else {
                return BSON(ChunkType::ns(collType.getNss().ns()));
            }
        }();
        writeToConfigDocumentInTxn(
            opCtx,
            ChunkType::ConfigNS,
            BatchedCommandRequest::buildPipelineUpdateOp(ChunkType::ConfigNS,
                                                         chunksQuery,
                                                         chunkUpdates,
                                                         false,  // upsert
                                                         true    // useMultiUpdate
                                                         ),
            txnNumber);

        LOGV2(21935,
              "refineCollectionShardKey: updated chunk entries for {namespace}: took "
              "{durationMillis} ms. Total time taken: {totalTimeMillis} ms.",
              "refineCollectionShardKey: updated chunk entries",
              "namespace"_attr = nss.ns(),
              "durationMillis"_attr = executionTimer.millis(),
              "totalTimeMillis"_attr = totalTimer.millis());
        executionTimer.reset();

        // Update all config.tags entries for the given namespace by setting their bounds for
        // each new field in the refined key to MinKey (except for the global max tag where the
        // max bounds are set to MaxKey).
        writeToConfigDocumentInTxn(
            opCtx,
            TagsType::ConfigNS,
            BatchedCommandRequest::buildPipelineUpdateOp(TagsType::ConfigNS,
                                                         BSON("ns" << nss.ns()),
                                                         tagUpdates,
                                                         false,  // upsert
                                                         true    // useMultiUpdate
                                                         ),
            txnNumber);


        LOGV2(21936,
              "refineCollectionShardKey: updated zone entries for {namespace}: took "
              "{durationMillis} ms. Total time taken: {totalTimeMillis} ms.",
              "refineCollectionShardKey: updated zone entries",
              "namespace"_attr = nss.ns(),
              "durationMillis"_attr = executionTimer.millis(),
              "totalTimeMillis"_attr = totalTimer.millis());

        if (MONGO_unlikely(hangRefineCollectionShardKeyBeforeCommit.shouldFail())) {
            LOGV2(21937, "Hit hangRefineCollectionShardKeyBeforeCommit failpoint");
            hangRefineCollectionShardKeyBeforeCommit.pauseWhileSet(opCtx);
        }
    };

    withTransaction(opCtx, nss, std::move(updateCollectionAndChunksFn));

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "refineCollectionShardKey.end",
                                           nss.ns(),
                                           BSONObj(),
                                           ShardingCatalogClient::kLocalWriteConcern);

    // Trigger refreshes on each shard containing chunks in the namespace 'nss'. Since this isn't
    // necessary for correctness, all refreshes are best-effort.
    try {
        triggerFireAndForgetShardRefreshes(opCtx, collType);
    } catch (const DBException& ex) {
        LOGV2(
            51798,
            "refineCollectionShardKey: failed to best-effort refresh all shards containing chunks "
            "in {namespace}",
            "refineCollectionShardKey: failed to best-effort refresh all shards containing chunks",
            "error"_attr = ex.toStatus(),
            "namespace"_attr = nss.ns());
    }
}

void ShardingCatalogManager::updateShardingCatalogEntryForCollectionInTxn(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionType& coll,
    const bool upsert,
    TxnNumber txnNumber) {
    try {
        writeToConfigDocumentInTxn(
            opCtx,
            CollectionType::ConfigNS,
            BatchedCommandRequest::buildUpdateOp(CollectionType::ConfigNS,
                                                 BSON(CollectionType::kNssFieldName << nss.ns()),
                                                 coll.toBSON(),
                                                 upsert,
                                                 false /* multi */
                                                 ),
            txnNumber);
    } catch (DBException& e) {
        e.addContext("Collection metadata write failed");
        throw;
    }
}

}  // namespace mongo
