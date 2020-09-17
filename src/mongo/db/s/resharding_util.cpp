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

#include "mongo/db/s/resharding_util.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {
using namespace fmt::literals;

namespace {

UUID getCollectionUuid(OperationContext* opCtx, const NamespaceString& nss) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IS));

    auto uuid = CollectionCatalog::get(opCtx).lookupUUIDByNSS(opCtx, nss);
    invariant(uuid);

    return *uuid;
}

// Assembles the namespace string for the temporary resharding collection based on the source
// namespace components.
NamespaceString getTempReshardingNss(StringData db, const UUID& sourceUuid) {
    return NamespaceString(db,
                           fmt::format("{}{}",
                                       NamespaceString::kTemporaryReshardingCollectionPrefix,
                                       sourceUuid.toString()));
}

// Ensure that this shard owns the document. This must be called after verifying that we
// are in a resharding operation so that we are guaranteed that migrations are suspended.
bool documentBelongsToMe(OperationContext* opCtx, CollectionShardingState* css, BSONObj doc) {
    auto currentKeyPattern = ShardKeyPattern(css->getCollectionDescription(opCtx).getKeyPattern());
    auto ownershipFilter = css->getOwnershipFilter(
        opCtx, CollectionShardingState::OrphanCleanupPolicy::kAllowOrphanCleanup);

    return ownershipFilter.keyBelongsToMe(currentKeyPattern.extractShardKeyFromDoc(doc));
}

boost::optional<TypeCollectionDonorFields> getDonorFields(OperationContext* opCtx,
                                                          const NamespaceString& sourceNss,
                                                          BSONObj fullDocument) {
    auto css = CollectionShardingState::get(opCtx, sourceNss);
    auto collDesc = css->getCollectionDescription(opCtx);

    if (!collDesc.isSharded())
        return boost::none;

    const auto& reshardingFields = collDesc.getReshardingFields();
    if (!reshardingFields)
        return boost::none;

    const auto& donorFields = reshardingFields->getDonorFields();
    if (!donorFields)
        return boost::none;

    if (!documentBelongsToMe(opCtx, css, fullDocument))
        return boost::none;

    return donorFields;
}

}  // namespace

UUID getCollectionUUIDFromChunkManger(const NamespaceString& originalNss, const ChunkManager& cm) {
    auto collectionUUID = cm.getUUID();
    uassert(ErrorCodes::InvalidUUID,
            "Cannot reshard collection {} due to missing UUID"_format(originalNss.ns()),
            collectionUUID);

    return collectionUUID.get();
}
NamespaceString constructTemporaryReshardingNss(const NamespaceString& originalNss,
                                                const ChunkManager& cm) {
    auto collectionUUID = getCollectionUUIDFromChunkManger(originalNss, cm);
    NamespaceString tempReshardingNss(
        originalNss.db(),
        "{}{}"_format(NamespaceString::kTemporaryReshardingCollectionPrefix,
                      collectionUUID.toString()));
    return tempReshardingNss;
}

BatchedCommandRequest buildInsertOp(const NamespaceString& nss, std::vector<BSONObj> docs) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments(docs);
        return insertOp;
    }());

    return request;
}

BatchedCommandRequest buildUpdateOp(const NamespaceString& nss,
                                    const BSONObj& query,
                                    const BSONObj& update,
                                    bool upsert,
                                    bool multi) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(upsert);
            entry.setMulti(multi);
            return entry;
        }()});
        return updateOp;
    }());

    return request;
}

BatchedCommandRequest buildDeleteOp(const NamespaceString& nss,
                                    const BSONObj& query,
                                    bool multiDelete) {
    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(multiDelete);
            return entry;
        }()});
        return deleteOp;
    }());

    return request;
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
                CollectionPtr coll = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(
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
            options.pipeline = BSON_ARRAY(getSlimOplogPipeline());
            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(
                db->createView(opCtx,
                               NamespaceString("local.system.resharding.slimOplogForGraphLookup"),
                               options));
            wuow.commit();
        });
}

BSONObj getSlimOplogPipeline() {
    return BSON("$project" << BSON(
                    "_id"
                    << "$ts"
                    << "op" << 1 << "o"
                    << BSON("applyOps" << BSON("ui" << 1 << "destinedRecipient" << 1)) << "ts" << 1
                    << "prevOpTime.ts"
                    << BSON("$cond" << BSON("if" << BSON("$eq" << BSON_ARRAY(BSON("$type"
                                                                                  << "$stmtId")
                                                                             << "missing"))
                                                 << "then"
                                                 << "$prevOpTime.ts"
                                                 << "else" << Timestamp::min()))));
}

std::unique_ptr<Pipeline, PipelineDeleter> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard,
    bool doesDonorOwnMinKeyChunk) {

    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;
    const Value EXISTS = V{Doc{{"$exists", true}}};
    const Value DNE = V{Doc{{"$exists", false}}};

    Pipeline::SourceContainer stages;
    // The node receiving the query verifies continuity of oplog entries (i.e: that the recipient
    // hasn't fallen off the oplog). This stage provides the input timestamp that the donor uses for
    // verification.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"ts", Doc{{"$gte", startAfter.getTs()}}}}.toBson(), expCtx));

    const Value captureCommandsOnCollectionClause = doesDonorOwnMinKeyChunk
        ? V{Doc{{"op", "c"_sd}, {"ui", collUUID}}}
        : V{Doc{{"op", "c"_sd}, {"ui", collUUID}, {"o.drop", EXISTS}}};

    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             // Only capture CRUD operations relevant for the `destinedRecipient`.
             Arr{V{Doc{{"op", Doc{{"$in", Arr{V{"i"_sd}, V{"u"_sd}, V{"d"_sd}, V{"n"_sd}}}}},
                       {"ui", collUUID},
                       {"destinedRecipient", recipientShard.toString()}}},
                 // Capture all commands. One cannot determine if a command is relevant to the
                 // `destinedRecipient` until after oplog chaining via `prevOpTime` is resolved.
                 V{Doc{{"op", "c"_sd},
                       {"o.applyOps", EXISTS},
                       {"o.partialTxn", DNE},
                       {"o.prepare", DNE}}},
                 V{Doc{{"op", "c"_sd}, {"o.commitTransaction", EXISTS}}},
                 V{Doc{{"op", "c"_sd}, {"o.abortTransaction", EXISTS}}},
                 captureCommandsOnCollectionClause}}}
            .toBson(),
        expCtx));

    // Denormalize oplog chaining. This will shove meta-information (particularly timestamps and
    // `destinedRecipient`) into the current aggregation output (still a raw oplog entry). This
    // meta-information is used for performing $lookups against the timestamp field and filtering
    // out earlier commands where the necessary `destinedRecipient` data wasn't yet available.
    stages.emplace_back(DocumentSourceGraphLookUp::create(
        expCtx,
        NamespaceString("local.system.resharding.slimOplogForGraphLookup"),  // from
        "history",                                                           // as
        "prevOpTime.ts",                                                     // connectFromField
        "ts",                                                                // connectToField
        ExpressionFieldPath::parse(expCtx.get(),
                                   "$ts",
                                   expCtx->variablesParseState),  // startWith
        boost::none,                                              // additionalFilter
        boost::optional<FieldPath>("depthForResharding"),         // depthField
        boost::none,                                              // maxDepth
        boost::none));                                            // unwindSrc

    // Only keep oplog entries for the relevant `destinedRecipient`.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             Arr{V{Doc{{"history", Doc{{"$size", 1}}},
                       {"$or",
                        Arr{V{Doc{{"history.0.op", Doc{{"$ne", "c"_sd}}}}},
                            V{Doc{{"history.0.op", "c"_sd}, {"history.0.o.applyOps", DNE}}}}}}},
                 V{Doc{{"history",
                        Doc{{"$elemMatch",
                             Doc{{"op", "c"_sd},
                                 {"o.applyOps",
                                  Doc{{"$elemMatch",
                                       Doc{{"ui", collUUID},
                                           {"destinedRecipient",
                                            recipientShard.toString()}}}}}}}}}}}}}}
            .toBson(),
        expCtx));

    // There's no guarantee to the order of entries accumulated in $graphLookup. The $reduce
    // expression sorts the `history` array in ascending `depthForResharding` order. The
    // $reverseArray expression will give an array in ascending timestamp order.
    stages.emplace_back(DocumentSourceAddFields::create(fromjson("{\
                    history: {$reverseArray: {$reduce: {\
                        input: '$history',\
                        initialValue: {$range: [0, {$size: '$history'}]},\
                        in: {$concatArrays: [\
                            {$slice: ['$$value', '$$this.depthForResharding']},\
                            ['$$this'],\
                            {$slice: [\
                                '$$value',\
                                {$subtract: [\
                                    {$add: ['$$this.depthForResharding', 1]},\
                                    {$size: '$history'}]}]}]}}}}}"),
                                                        expCtx));

    // If the last entry in the history is an `abortTransaction`, leave the `abortTransaction` oplog
    // entry in place, but remove all prior `applyOps` entries. The `abortTransaction` entry is
    // required to update the `config.transactions` table. Removing the `applyOps` entries ensures
    // we don't make any data writes that would have to be undone.
    stages.emplace_back(DocumentSourceAddFields::create(fromjson("{\
                        'history': {$let: {\
                            vars: {lastEntry: {$arrayElemAt: ['$history', -1]}},\
                            in: {$cond: {\
                                if: {$and: [\
                                    {$eq: ['$$lastEntry.op', 'c']},\
                                    {$ne: [{$type: '$$lastEntry.o.abortTransaction'}, 'missing']}\
                                ]},\
                                then: ['$$lastEntry'],\
                                else: '$history'}}}}}"),
                                                        expCtx));

    // Unwind the history array. The output at this point is a new stream of oplog entries, each
    // with exactly one history element. If there are no multi-oplog transactions (e.g: large
    // transactions, prepared transactions), the documents will be in timestamp order. In the
    // presence of large or prepared transactions, the data writes that were part of prior oplog
    // entries will be adjacent to each other, terminating with a `commitTransaction` oplog entry.
    stages.emplace_back(DocumentSourceUnwind::create(expCtx, "history", false, boost::none));

    // Group the relevant timestamps into an `_id` field. The `_id.clusterTime` value is the
    // timestamp of the last entry in a multi-oplog entry transaction. The `_id.ts` value is the
    // timestamp of the oplog entry that operation appeared in. For typical CRUD operations, these
    // are the same. In multi-oplog entry transactions, `_id.clusterTime` may be later than
    // `_id.ts`.
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        fromjson("{$replaceRoot: {newRoot: {$mergeObjects: [\
                     '$history',\
                     {_id: {clusterTime: '$ts', ts: '$history.ts'}}]}}}")
            .firstElement(),
        expCtx));

    // Now that the chained oplog entries are adjacent with an annotated `ReshardingDonorOplogId`,
    // the pipeline can prune anything earlier than the resume time.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"_id", Doc{{"$gt", startAfter.toBSON()}}}}.toBson(), expCtx));

    // Using the `ts` field, attach the full oplog document. Note that even for simple oplog
    // entries, the oplog contents were thrown away making this step necessary for all documents.
    stages.emplace_back(DocumentSourceLookUp::createFromBson(Doc{{"$lookup",
                                                                  Doc{{"from", "oplog.rs"_sd},
                                                                      {"localField", "ts"_sd},
                                                                      {"foreignField", "ts"_sd},
                                                                      {"as", "fullEntry"_sd}}}}
                                                                 .toBson()
                                                                 .firstElement(),
                                                             expCtx));

    // The outer fields of the pipeline document only contain meta-information about the
    // operation. The prior `$lookup` places the actual operations into a `fullEntry` array of size
    // one (timestamps are unique, thus always exactly one value).
    stages.emplace_back(DocumentSourceUnwind::create(expCtx, "fullEntry", false, boost::none));

    // Keep only the oplog entry from the `$lookup` merged with the `_id`.
    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        fromjson("{$replaceRoot: {newRoot: {$mergeObjects: ['$fullEntry', {_id: '$_id'}]}}}")
            .firstElement(),
        expCtx));

    // Filter out anything inside of an `applyOps` specifically destined for another shard. This
    // ensures zone restrictions are obeyed. Data will never be sent to a shard that it isn't meant
    // to end up on.
    stages.emplace_back(DocumentSourceAddFields::create(
        Doc{{"o.applyOps",
             Doc{{"$cond",
                  Doc{{"if", Doc{{"$eq", Arr{V{"$op"_sd}, V{"c"_sd}}}}},
                      {"then",
                       Doc{{"$filter",
                            Doc{{"input", "$o.applyOps"_sd},
                                {"cond",
                                 Doc{{"$and",
                                      Arr{V{Doc{{"$eq", Arr{V{"$$this.ui"_sd}, V{collUUID}}}}},
                                          V{Doc{{"$eq",
                                                 Arr{V{"$$this.destinedRecipient"_sd},
                                                     V{recipientShard.toString()}}}}}}}}}}}}},
                      {"else", "$o.applyOps"_sd}}}}}}
            .toBson(),
        expCtx));

    return Pipeline::create(std::move(stages), expCtx);
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
                if (ShardKeyPattern::isHashedPatternEl(field)) {
                    skVarBuilder.append(BSON("$toHashedIndexKey"
                                             << "$original." + field.fieldNameStringData()));
                } else {
                    skVarBuilder.append("$original." + field.fieldNameStringData());
                }
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
                                                                                  << "maxKey"))))))
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

boost::optional<ShardId> getDestinedRecipient(OperationContext* opCtx,
                                              const NamespaceString& sourceNss,
                                              BSONObj fullDocument) {
    auto donorFields = getDonorFields(opCtx, sourceNss, fullDocument);
    if (!donorFields)
        return boost::none;

    bool allowLocks = true;
    auto tempNssRoutingInfo = Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
        opCtx,
        getTempReshardingNss(sourceNss.db(), getCollectionUuid(opCtx, sourceNss)),
        allowLocks);

    uassert(ErrorCodes::ShardInvalidatedForTargeting,
            "Routing information is not available for the temporary resharding collection.",
            tempNssRoutingInfo.getStatus() != ErrorCodes::StaleShardVersion);

    uassertStatusOK(tempNssRoutingInfo);

    auto reshardingKeyPattern = ShardKeyPattern(donorFields->getReshardingKey());
    auto shardKey = reshardingKeyPattern.extractShardKeyFromDoc(fullDocument);

    return tempNssRoutingInfo.getValue()
        .findIntersectingChunkWithSimpleCollation(shardKey)
        .getShardId();
}

}  // namespace mongo
