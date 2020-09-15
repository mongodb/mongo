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

#include "mongo/platform/basic.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"

namespace mongo {
using namespace fmt::literals;

NamespaceString constructTemporaryReshardingNss(const NamespaceString& originalNss,
                                                const ChunkManager& cm) {
    auto collectionUUID = cm.getUUID();
    uassert(ErrorCodes::InvalidUUID,
            "Cannot reshard collection {} due to missing UUID"_format(originalNss.ns()),
            collectionUUID);
    NamespaceString tempReshardingNss(
        originalNss.db(),
        "{}{}"_format(NamespaceString::kTemporaryReshardingCollectionPrefix,
                      collectionUUID->toString()));
    return tempReshardingNss;
}

void tellShardsToRefresh(OperationContext* opCtx,
                         const std::vector<ShardId>& shardIds,
                         const NamespaceString& nss,
                         std::shared_ptr<executor::TaskExecutor> executor) {
    auto cmd = _flushRoutingTableCacheUpdatesWithWriteConcern(nss);
    cmd.setSyncFromConfig(true);
    cmd.setDbName(nss.db());
    auto cmdObj =
        cmd.toBSON(BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));

    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        requests.emplace_back(shardId, cmdObj);
    }

    if (!requests.empty()) {
        AsyncRequestsSender ars(opCtx,
                                executor,
                                "admin",
                                requests,
                                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                Shard::RetryPolicy::kIdempotent);

        while (!ars.done()) {
            // Retrieve the responses and throw at the first failure.
            auto response = ars.next();

            auto generateErrorContext = [&]() -> std::string {
                return str::stream()
                    << "Unable to _flushRoutingTableCacheUpdatesWithWriteConcern for namespace "
                    << nss.ns() << " on " << response.shardId;
            };

            auto shardResponse =
                uassertStatusOKWithContext(std::move(response.swResponse), generateErrorContext());

            auto status = getStatusFromCommandResult(shardResponse.data);
            uassertStatusOKWithContext(status, generateErrorContext());

            auto wcStatus = getWriteConcernStatusFromCommandResult(shardResponse.data);
            uassertStatusOKWithContext(wcStatus, generateErrorContext());
        }
    }
}

void checkForHolesAndOverlapsInChunks(std::vector<ReshardedChunk>& chunks,
                                      const KeyPattern& keyPattern) {
    std::sort(chunks.begin(), chunks.end(), [](const ReshardedChunk& a, const ReshardedChunk& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
    });
    // Check for global minKey and maxKey
    uassert(ErrorCodes::BadValue,
            "Chunk range must start at global min for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.front().getMin() ==
                                                        keyPattern.globalMin()));
    uassert(ErrorCodes::BadValue,
            "Chunk range must end at global max for new shard key",
            SimpleBSONObjComparator::kInstance.evaluate(chunks.back().getMax() ==
                                                        keyPattern.globalMax()));

    boost::optional<BSONObj> prevMax = boost::none;
    for (auto chunk : chunks) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Chunk ranges must be contiguous",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.get() == chunk.getMin()));
        }
        prevMax = boost::optional<BSONObj>(chunk.getMax());
    }
}

void validateReshardedChunks(const std::vector<mongo::BSONObj>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern) {
    std::vector<ReshardedChunk> validChunks;
    for (const BSONObj& obj : chunks) {
        auto chunk = ReshardedChunk::parse(IDLParserErrorContext("reshardedChunks"), obj);
        uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getRecipientShardId()));
        validChunks.push_back(chunk);
    }

    checkForHolesAndOverlapsInChunks(validChunks, keyPattern);
}

void checkForOverlappingZones(std::vector<TagsType>& zones) {
    std::sort(zones.begin(), zones.end(), [](const TagsType& a, const TagsType& b) {
        return SimpleBSONObjComparator::kInstance.evaluate(a.getMinKey() < b.getMinKey());
    });

    boost::optional<BSONObj> prevMax = boost::none;
    for (auto zone : zones) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Zone ranges must not overlap",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.get() <= zone.getMinKey()));
        }
        prevMax = boost::optional<BSONObj>(zone.getMaxKey());
    }
}

void validateZones(const std::vector<mongo::BSONObj>& zones,
                   const std::vector<TagsType>& authoritativeTags) {
    std::vector<TagsType> validZones;

    for (const BSONObj& obj : zones) {
        auto zone = uassertStatusOK(TagsType::fromBSON(obj));
        auto zoneName = zone.getTag();
        auto it =
            std::find_if(authoritativeTags.begin(),
                         authoritativeTags.end(),
                         [&zoneName](const TagsType& obj) { return obj.getTag() == zoneName; });
        uassert(ErrorCodes::BadValue, "Zone must already exist", it != authoritativeTags.end());
        validZones.push_back(zone);
    }

    checkForOverlappingZones(validZones);
}

std::unique_ptr<Pipeline, PipelineDeleter> createAggForReshardingOplogBuffer(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::optional<ReshardingDonorOplogId>& resumeToken) {
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    if (resumeToken) {
        stages.emplace_back(DocumentSourceMatch::create(
            BSON("_id" << BSON("$gt" << resumeToken->toBSON())), expCtx));
    }

    stages.emplace_back(DocumentSourceSort::create(expCtx, BSON("_id" << 1)));

    BSONObjBuilder lookupBuilder;
    lookupBuilder.append("from", expCtx->ns.coll());
    lookupBuilder.append("let",
                         BSON("preImageId" << BSON("clusterTime"
                                                   << "$preImageOpTime.ts"
                                                   << "ts"
                                                   << "$preImageOpTime.ts")
                                           << "postImageId"
                                           << BSON("clusterTime"
                                                   << "$postImageOpTime.ts"
                                                   << "ts"
                                                   << "$postImageOpTime.ts")));
    lookupBuilder.append("as", kReshardingOplogPrePostImageOps);

    BSONArrayBuilder lookupPipelineBuilder(lookupBuilder.subarrayStart("pipeline"));
    lookupPipelineBuilder.append(
        BSON("$match" << BSON(
                 "$expr" << BSON("$in" << BSON_ARRAY("$_id" << BSON_ARRAY("$$preImageId"
                                                                          << "$$postImageId"))))));
    lookupPipelineBuilder.done();

    BSONObj lookupBSON(BSON("" << lookupBuilder.obj()));
    stages.emplace_back(DocumentSourceLookUp::createFromBson(lookupBSON.firstElement(), expCtx));

    return Pipeline::create(std::move(stages), expCtx);
}

std::unique_ptr<Pipeline, PipelineDeleter> createConfigTxnCloningPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter) {
    invariant(!fetchTimestamp.isNull());

    std::list<boost::intrusive_ptr<DocumentSource>> stages;
    if (startAfter) {
        stages.emplace_back(DocumentSourceMatch::create(
            BSON("_id" << BSON("$gt" << startAfter->toBSON())), expCtx));
    }
    stages.emplace_back(DocumentSourceSort::create(expCtx, BSON("_id" << 1)));
    stages.emplace_back(DocumentSourceMatch::create(
        BSON("lastWriteOpTime.ts" << BSON("$lt" << fetchTimestamp)), expCtx));

    return Pipeline::create(std::move(stages), expCtx);
}

void createSlimOplogView(OperationContext* opCtx, Database* db) {
    writeConflictRetry(
        opCtx, "createReshardingOplog", "local.system.resharding.slimOplogForGraphLookup", [&] {
            {
                // Create 'system.views' in a separate WUOW if it does not exist.
                WriteUnitOfWork wuow(opCtx);
                const Collection* coll = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(
                    opCtx, NamespaceString(db->getSystemViewsName()));
                if (!coll) {
                    coll = db->createCollection(opCtx, NamespaceString(db->getSystemViewsName()));
                }
                invariant(coll);
                wuow.commit();
            }

            // Resharding uses the `prevOpTime` to link oplog related entries via a
            // $graphLookup. Large transactions and prepared transaction use prevOpTime to identify
            // earlier oplog entries from the same transaction. Retryable writes (identified via the
            // presence of `stmtId`) use prevOpTime to identify earlier run statements from the same
            // retryable write.  This view will unlink oplog entries from the same retryable write
            // by zeroing out their `prevOpTime`.
            CollectionOptions options;
            options.viewOn = NamespaceString::kRsOplogNamespace.coll().toString();
            options.pipeline = BSON_ARRAY(BSON(
                "$project" << BSON(
                    "_id"
                    << "$ts"
                    << "op" << 1 << "o" << BSON("applyOps" << BSON("ui" << 1 << "reshardDest" << 1))
                    << "ts" << 1 << "prevOpTime.ts"
                    << BSON("$cond" << BSON("if" << BSON("$eq" << BSON_ARRAY(BSON("$type"
                                                                                  << "$stmtId")
                                                                             << "missing"))
                                                 << "then"
                                                 << "$prevOpTime.ts"
                                                 << "else" << Timestamp::min())))));
            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(
                db->createView(opCtx,
                               NamespaceString("local.system.resharding.slimOplogForGraphLookup"),
                               options));
            wuow.commit();
        });
}

std::unique_ptr<Pipeline, PipelineDeleter> createAggForCollectionCloning(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ShardKeyPattern& newShardKeyPattern,
    const NamespaceString& tempNss,
    const ShardId& recipientShard) {
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    BSONObj replaceWithBSON = BSON("$replaceWith" << BSON("original"
                                                          << "$$ROOT"));
    stages.emplace_back(
        DocumentSourceReplaceRoot::createFromBson(replaceWithBSON.firstElement(), expCtx));

    invariant(tempNss.isTemporaryReshardingCollection());
    std::string cacheChunksColl = "cache.chunks." + tempNss.toString();
    BSONObjBuilder lookupBuilder;
    lookupBuilder.append("from",
                         BSON("db"
                              << "config"
                              << "coll" << cacheChunksColl));
    {
        BSONObjBuilder letBuilder(lookupBuilder.subobjStart("let"));
        {
            BSONArrayBuilder skVarBuilder(letBuilder.subarrayStart("sk"));
            for (auto&& field : newShardKeyPattern.toBSON()) {
                skVarBuilder.append("$original." + field.fieldNameStringData());
            }
        }
    }
    BSONArrayBuilder lookupPipelineBuilder(lookupBuilder.subarrayStart("pipeline"));
    lookupPipelineBuilder.append(
        BSON("$match" << BSON(
                 "$expr" << BSON("$eq" << BSON_ARRAY(recipientShard.toString() << "$shard")))));
    lookupPipelineBuilder.append(BSON(
        "$match" << BSON(
            "$expr" << BSON(
                "$let" << BSON(
                    "vars" << BSON("min" << BSON("$map" << BSON("input" << BSON("$objectToArray"
                                                                                << "$_id")
                                                                        << "in"
                                                                        << "$$this.v"))
                                         << "max"
                                         << BSON("$map" << BSON("input" << BSON("$objectToArray"
                                                                                << "$max")
                                                                        << "in"
                                                                        << "$$this.v")))
                           << "in"
                           << BSON(
                                  "$and" << BSON_ARRAY(
                                      BSON("$gte" << BSON_ARRAY("$$sk"
                                                                << "$$min"))
                                      << BSON("$cond" << BSON(
                                                  "if"
                                                  << BSON("$allElementsTrue" << BSON_ARRAY(BSON(
                                                              "$map"
                                                              << BSON("input"
                                                                      << "$$max"
                                                                      << "in"
                                                                      << BSON("$eq" << BSON_ARRAY(
                                                                                  BSON("$type"
                                                                                       << "$$this")
                                                                                  << "$maxKey"))))))
                                                  << "then"
                                                  << BSON("$lte" << BSON_ARRAY("$$sk"
                                                                               << "$$max"))
                                                  << "else"
                                                  << BSON("$lt" << BSON_ARRAY("$$sk"
                                                                              << "$$max")))))))))));

    lookupPipelineBuilder.done();
    lookupBuilder.append("as", "intersectingChunk");
    BSONObj lookupBSON(BSON("" << lookupBuilder.obj()));
    stages.emplace_back(DocumentSourceLookUp::createFromBson(lookupBSON.firstElement(), expCtx));
    stages.emplace_back(DocumentSourceMatch::create(
        BSON("intersectingChunk" << BSON("$ne" << BSONArray())), expCtx));
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(BSON("$replaceWith"
                                                                       << "$original")
                                                                      .firstElement(),
                                                                  expCtx));

    return Pipeline::create(std::move(stages), expCtx);
}

}  // namespace mongo
