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


#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_util.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/resharding/document_source_resharding_add_resume_id.h"
#include "mongo/db/s/resharding/document_source_resharding_iterate_transaction.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/shard_invalidated_for_targeting_exception.h"
#include "mongo/s/shard_key_pattern.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {
namespace resharding {

namespace {
/**
 * Given a constant rate of time per unit of work:
 *    totalTime / totalWork == elapsedTime / elapsedWork
 * Solve for remaining time.
 *    remainingTime := totalTime - elapsedTime
 *                  == (totalWork * (elapsedTime / elapsedWork)) - elapsedTime
 *                  == elapsedTime * (totalWork / elapsedWork - 1)
 */
Milliseconds estimateRemainingTime(Milliseconds elapsedTime, double elapsedWork, double totalWork) {
    elapsedWork = std::min(elapsedWork, totalWork);
    double remainingMsec = 1.0 * elapsedTime.count() * (totalWork / elapsedWork - 1);
    return Milliseconds(Milliseconds::rep(std::round(remainingMsec)));
}
}  // namespace

using namespace fmt::literals;

BSONObj serializeAndTruncateReshardingErrorIfNeeded(Status originalError) {
    BSONObjBuilder originalBob;
    originalError.serializeErrorToBSON(&originalBob);
    auto originalObj = originalBob.obj();

    if (originalObj.objsize() <= kReshardErrorMaxBytes ||
        originalError.code() == ErrorCodes::ReshardCollectionTruncatedError) {
        // The provided originalError either meets the size constraints or has already been
        // truncated (and is just slightly larger than 2000 bytes to avoid complicating the
        // truncation math).
        return originalObj;
    }

    // ReshardCollectionAborted has special internal handling. It should always have a short, fixed
    // error message so it never exceeds the size limit and requires truncation and error code
    // substitution.
    invariant(originalError.code() != ErrorCodes::ReshardCollectionAborted);

    auto originalErrorStr = originalError.toString();
    auto truncatedErrorStr =
        str::UTF8SafeTruncation(StringData(originalErrorStr), kReshardErrorMaxBytes);
    Status truncatedError{ErrorCodes::ReshardCollectionTruncatedError, truncatedErrorStr};
    BSONObjBuilder truncatedBob;
    truncatedError.serializeErrorToBSON(&truncatedBob);
    return truncatedBob.obj();
}

DonorShardEntry makeDonorShard(ShardId shardId,
                               DonorStateEnum donorState,
                               boost::optional<Timestamp> minFetchTimestamp,
                               boost::optional<Status> abortReason) {
    DonorShardContext donorCtx;
    donorCtx.setState(donorState);
    emplaceMinFetchTimestampIfExists(donorCtx, minFetchTimestamp);
    emplaceTruncatedAbortReasonIfExists(donorCtx, abortReason);

    return DonorShardEntry{std::move(shardId), std::move(donorCtx)};
}

RecipientShardEntry makeRecipientShard(ShardId shardId,
                                       RecipientStateEnum recipientState,
                                       boost::optional<Status> abortReason) {
    RecipientShardContext recipientCtx;
    recipientCtx.setState(recipientState);
    emplaceTruncatedAbortReasonIfExists(recipientCtx, abortReason);

    return RecipientShardEntry{std::move(shardId), std::move(recipientCtx)};
}

NamespaceString constructTemporaryReshardingNss(StringData db, const UUID& sourceUuid) {
    return NamespaceString(db,
                           fmt::format("{}{}",
                                       NamespaceString::kTemporaryReshardingCollectionPrefix,
                                       sourceUuid.toString()));
}

std::set<ShardId> getRecipientShards(OperationContext* opCtx,
                                     const NamespaceString& sourceNss,
                                     const UUID& reshardingUUID) {
    const auto& tempNss = constructTemporaryReshardingNss(sourceNss.db(), reshardingUUID);
    auto* catalogCache = Grid::get(opCtx)->catalogCache();
    auto cm = uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, tempNss));

    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Expected collection " << tempNss << " to be sharded",
            cm.isSharded());

    std::set<ShardId> recipients;
    cm.getAllShardIds(&recipients);
    return recipients;
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

void validateReshardedChunks(const std::vector<ReshardedChunk>& chunks,
                             OperationContext* opCtx,
                             const KeyPattern& keyPattern) {
    std::vector<ReshardedChunk> validChunks;
    for (const auto& chunk : chunks) {
        uassertStatusOK(
            Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk.getRecipientShardId()));
        validChunks.push_back(chunk);
    }
    checkForHolesAndOverlapsInChunks(validChunks, keyPattern);
}

Timestamp getHighestMinFetchTimestamp(const std::vector<DonorShardEntry>& donorShards) {
    invariant(!donorShards.empty());

    auto maxMinFetchTimestamp = Timestamp::min();
    for (auto& donor : donorShards) {
        auto donorFetchTimestamp = donor.getMutableState().getMinFetchTimestamp();
        uassert(4957300,
                "All donors must have a minFetchTimestamp, but donor {} does not."_format(
                    StringData{donor.getId()}),
                donorFetchTimestamp.is_initialized());
        if (maxMinFetchTimestamp < donorFetchTimestamp.value()) {
            maxMinFetchTimestamp = donorFetchTimestamp.value();
        }
    }
    return maxMinFetchTimestamp;
}

void checkForOverlappingZones(std::vector<ReshardingZoneType>& zones) {
    std::sort(
        zones.begin(), zones.end(), [](const ReshardingZoneType& a, const ReshardingZoneType& b) {
            return SimpleBSONObjComparator::kInstance.evaluate(a.getMin() < b.getMin());
        });

    boost::optional<BSONObj> prevMax = boost::none;
    for (auto zone : zones) {
        if (prevMax) {
            uassert(ErrorCodes::BadValue,
                    "Zone ranges must not overlap",
                    SimpleBSONObjComparator::kInstance.evaluate(prevMax.get() <= zone.getMin()));
        }
        prevMax = boost::optional<BSONObj>(zone.getMax());
    }
}

std::vector<BSONObj> buildTagsDocsFromZones(const NamespaceString& tempNss,
                                            const std::vector<ReshardingZoneType>& zones) {
    std::vector<BSONObj> tags;
    tags.reserve(zones.size());
    for (const auto& zone : zones) {
        ChunkRange range(zone.getMin(), zone.getMax());
        TagsType tag(tempNss, zone.getZone().toString(), range);
        tags.push_back(tag.toBSON());
    }

    return tags;
}

std::unique_ptr<Pipeline, PipelineDeleter> createOplogFetchingPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ReshardingDonorOplogId& startAfter,
    UUID collUUID,
    const ShardId& recipientShard) {
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
                 V{Doc{{"op", "c"_sd}, {"ui", collUUID}}}}}}
            .toBson(),
        expCtx));

    // Emits transaction entries chronologically.
    stages.emplace_back(DocumentSourceReshardingIterateTransaction::create(
        expCtx, true /* includeCommitTransactionTimestamp */));

    // Converts oplog entries with kNeedsRetryImageFieldName into the old style pair of
    // update/delete oplog and pre/post image no-op oplog.
    stages.emplace_back(DocumentSourceFindAndModifyImageLookup::create(
        expCtx, true /* includeCommitTransactionTimestamp */));

    // Adds _id to all events in the stream.
    stages.emplace_back(DocumentSourceReshardingAddResumeId::create(expCtx));

    // Filter out applyOps entries which do not contain any relevant operations.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"$or",
             Arr{V{Doc{{"op", Doc{{"$ne", "c"_sd}}}}},
                 V{Doc{{"op", "c"_sd}, {"o.applyOps", DNE}}},
                 V{Doc{{"op", "c"_sd},
                       {"o.applyOps",
                        Doc{{"$elemMatch",
                             Doc{{"destinedRecipient", recipientShard.toString()},
                                 {"ui", collUUID}}}}}}}}}}
            .toBson(),
        expCtx));

    // Now that the chained oplog entries are adjacent with an annotated `ReshardingDonorOplogId`,
    // the pipeline can prune anything earlier than the resume time.
    stages.emplace_back(DocumentSourceMatch::create(
        Doc{{"_id", Doc{{"$gt", startAfter.toBSON()}}}}.toBson(), expCtx));

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

bool isFinalOplog(const repl::OplogEntry& oplog) {
    if (oplog.getOpType() != repl::OpTypeEnum::kNoop) {
        return false;
    }

    auto o2Field = oplog.getObject2();
    if (!o2Field) {
        return false;
    }

    return o2Field->getField("type").valueStringDataSafe() == kReshardFinalOpLogType;
}

bool isFinalOplog(const repl::OplogEntry& oplog, UUID reshardingUUID) {
    if (!isFinalOplog(oplog)) {
        return false;
    }

    return uassertStatusOK(UUID::parse(oplog.getObject2()->getField("reshardingUUID"))) ==
        reshardingUUID;
}

NamespaceString getLocalOplogBufferNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString("config.localReshardingOplogBuffer.{}.{}"_format(
        existingUUID.toString(), donorShardId.toString()));
}

NamespaceString getLocalConflictStashNamespace(UUID existingUUID, ShardId donorShardId) {
    return NamespaceString{NamespaceString::kConfigDb,
                           "localReshardingConflictStash.{}.{}"_format(existingUUID.toString(),
                                                                       donorShardId.toString())};
}

void doNoopWrite(OperationContext* opCtx, StringData opStr, const NamespaceString& nss) {
    writeConflictRetry(opCtx, opStr, NamespaceString::kRsOplogNamespace.ns(), [&] {
        AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);

        const std::string msg = str::stream() << opStr << " on " << nss;
        WriteUnitOfWork wuow(opCtx);
        opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
            opCtx,
            {},
            boost::none,
            BSON("msg" << msg),
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            boost::none);
        wuow.commit();
    });
}

boost::optional<Milliseconds> estimateRemainingRecipientTime(bool applyingBegan,
                                                             int64_t bytesCopied,
                                                             int64_t bytesToCopy,
                                                             Milliseconds timeSpentCopying,
                                                             int64_t oplogEntriesApplied,
                                                             int64_t oplogEntriesFetched,
                                                             Milliseconds timeSpentApplying) {
    if (applyingBegan && oplogEntriesFetched == 0) {
        return Milliseconds(0);
    }
    if (oplogEntriesApplied > 0 && oplogEntriesFetched > 0) {
        // All fetched oplogEntries must be applied. Some of them already have been.
        return estimateRemainingTime(timeSpentApplying, oplogEntriesApplied, oplogEntriesFetched);
    }
    if (bytesCopied > 0 && bytesToCopy > 0) {
        // Until the time to apply batches of oplog entries is measured, we assume that applying all
        // of them will take as long as copying did.
        return estimateRemainingTime(timeSpentCopying, bytesCopied, 2 * bytesToCopy);
    }
    return {};
}

}  // namespace resharding
}  // namespace mongo
