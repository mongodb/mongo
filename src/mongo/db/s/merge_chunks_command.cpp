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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/merge_chunk_request_type.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

bool checkMetadataForSuccess(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const OID& epoch,
                             const ChunkRange& chunkRange) {
    const auto metadataAfterMerge = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    }();

    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "Collection " << nss.ns() << " changed since merge start",
            metadataAfterMerge->getCollVersion().epoch() == epoch);

    ChunkType chunk;
    if (!metadataAfterMerge->getNextChunk(chunkRange.getMin(), &chunk)) {
        return false;
    }

    return chunk.getMin().woCompare(chunkRange.getMin()) == 0 &&
        chunk.getMax().woCompare(chunkRange.getMax()) == 0;
}

void mergeChunks(OperationContext* opCtx,
                 const NamespaceString& nss,
                 const BSONObj& minKey,
                 const BSONObj& maxKey,
                 const OID& epoch) {
    const std::string whyMessage = str::stream() << "merging chunks in " << nss.ns() << " from "
                                                 << minKey << " to " << maxKey;
    auto scopedDistLock = uassertStatusOKWithContext(
        Grid::get(opCtx)->catalogClient()->getDistLockManager()->lock(
            opCtx, nss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout),
        str::stream() << "could not acquire collection lock for " << nss.ns()
                      << " to merge chunks in ["
                      << redact(minKey)
                      << ", "
                      << redact(maxKey)
                      << ")");

    auto const shardingState = ShardingState::get(opCtx);

    // We now have the collection distributed lock, refresh metadata to latest version and sanity
    // check
    forceShardFilteringMetadataRefresh(opCtx, nss);

    const auto metadata = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    }();

    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "Collection " << nss.ns() << " became unsharded",
            metadata->isSharded());

    const auto shardVersion = metadata->getShardVersion();
    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "could not merge chunks, collection " << nss.ns()
                          << " has changed since merge was sent (sent epoch: "
                          << epoch.toString()
                          << ", current epoch: "
                          << shardVersion.epoch()
                          << ")",
            shardVersion.epoch() == epoch);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, the range "
                          << redact(ChunkRange(minKey, maxKey).toString())
                          << " is not valid"
                          << " for collection "
                          << nss.ns()
                          << " with key pattern "
                          << metadata->getKeyPattern().toString(),
            metadata->isValidKey(minKey) && metadata->isValidKey(maxKey));

    //
    // Get merged chunk information
    //
    std::vector<ChunkType> chunksToMerge;
    std::vector<BSONObj> chunkBoundaries;
    chunkBoundaries.push_back(minKey);

    ChunkType itChunk;
    itChunk.setMin(minKey);
    itChunk.setMax(minKey);

    while (itChunk.getMax().woCompare(maxKey) < 0 &&
           metadata->getNextChunk(itChunk.getMax(), &itChunk)) {
        chunkBoundaries.push_back(itChunk.getMax());
        chunksToMerge.push_back(itChunk);
    }

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, collection " << nss.ns()
                          << " range starting at "
                          << redact(minKey)
                          << " and ending at "
                          << redact(maxKey)
                          << " does not belong to shard "
                          << shardingState->shardId(),
            !chunksToMerge.empty());

    //
    // Validate the range starts and ends at chunks and has no holes, error if not valid
    //

    BSONObj firstDocMin = chunksToMerge.front().getMin();
    BSONObj firstDocMax = chunksToMerge.front().getMax();
    // minKey is inclusive
    bool minKeyInRange = rangeContains(firstDocMin, firstDocMax, minKey);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, collection " << nss.ns()
                          << " range starting at "
                          << redact(minKey)
                          << " does not belong to shard "
                          << shardingState->shardId(),
            minKeyInRange);

    BSONObj lastDocMin = chunksToMerge.back().getMin();
    BSONObj lastDocMax = chunksToMerge.back().getMax();
    // maxKey is exclusive
    bool maxKeyInRange = lastDocMin.woCompare(maxKey) < 0 && lastDocMax.woCompare(maxKey) >= 0;

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, collection " << nss.ns()
                          << " range ending at "
                          << redact(maxKey)
                          << " does not belong to shard "
                          << shardingState->shardId(),
            maxKeyInRange);

    bool validRangeStartKey = firstDocMin.woCompare(minKey) == 0;
    bool validRangeEndKey = lastDocMax.woCompare(maxKey) == 0;

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, collection " << nss.ns()
                          << " does not contain a chunk "
                          << (!validRangeStartKey ? "starting at " + redact(minKey.toString()) : "")
                          << (!validRangeStartKey && !validRangeEndKey ? " or " : "")
                          << (!validRangeEndKey ? "ending at " + redact(maxKey.toString()) : ""),
            validRangeStartKey && validRangeEndKey);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, collection " << nss.ns()
                          << " already contains chunk for "
                          << ChunkRange(minKey, maxKey).toString(),
            chunksToMerge.size() > 1);

    // Look for hole in range
    for (size_t i = 1; i < chunksToMerge.size(); ++i) {
        uassert(
            ErrorCodes::IllegalOperation,
            str::stream()
                << "could not merge chunks, collection "
                << nss.ns()
                << " has a hole in the range "
                << ChunkRange(minKey, maxKey).toString()
                << " at "
                << ChunkRange(chunksToMerge[i - 1].getMax(), chunksToMerge[i].getMin()).toString(),
            chunksToMerge[i - 1].getMax().woCompare(chunksToMerge[i].getMin()) == 0);
    }

    //
    // Run _configsvrCommitChunkMerge.
    //
    MergeChunkRequest request{nss,
                              shardingState->shardId().toString(),
                              shardVersion.epoch(),
                              chunkBoundaries,
                              LogicalClock::get(opCtx)->getClusterTime().asTimestamp()};

    auto configCmdObj =
        request.toConfigCommandBSON(ShardingCatalogClient::kMajorityWriteConcern.toBSON());
    auto cmdResponse =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            configCmdObj,
            Shard::RetryPolicy::kIdempotent));

    // Refresh metadata to pick up new chunk definitions (regardless of the results returned from
    // running _configsvrCommitChunkMerge).
    forceShardFilteringMetadataRefresh(opCtx, nss);

    // If _configsvrCommitChunkMerge returned an error, look at this shard's metadata to determine
    // if the merge actually did happen. This can happen if there's a network error getting the
    // response from the first call to _configsvrCommitChunkMerge, but it actually succeeds, thus
    // the automatic retry fails with a precondition violation, for example.
    auto commandStatus = std::move(cmdResponse.commandStatus);
    auto writeConcernStatus = std::move(cmdResponse.writeConcernStatus);

    if ((!commandStatus.isOK() || !writeConcernStatus.isOK()) &&
        checkMetadataForSuccess(opCtx, nss, epoch, ChunkRange(minKey, maxKey))) {
        LOG(1) << "mergeChunk [" << redact(minKey) << "," << redact(maxKey)
               << ") has already been committed.";
        return;
    }

    uassertStatusOKWithContext(commandStatus, "Failed to commit chunk merge");
    uassertStatusOKWithContext(writeConcernStatus, "Failed to commit chunk merge");
}

class MergeChunksCommand : public ErrmsgCommandDeprecated {
public:
    MergeChunksCommand() : ErrmsgCommandDeprecated("mergeChunks") {}

    std::string help() const override {
        return "Internal command to merge a contiguous range of chunks.\n"
               "Usage: { mergeChunks: <ns>, epoch: <epoch>, bounds: [<min key>, <max key>] }";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    // Required
    static BSONField<std::string> nsField;
    static BSONField<std::vector<BSONObj>> boundsField;

    // Optional, if the merge is only valid for a particular epoch
    static BSONField<OID> epochField;

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

        const NamespaceString nss(parseNs(dbname, cmdObj));

        std::vector<BSONObj> bounds;
        if (!FieldParser::extract(cmdObj, boundsField, &bounds, &errmsg)) {
            return false;
        }

        if (bounds.size() == 0) {
            errmsg = "no bounds were specified";
            return false;
        }

        if (bounds.size() != 2) {
            errmsg = "only a min and max bound may be specified";
            return false;
        }

        BSONObj minKey = bounds[0];
        BSONObj maxKey = bounds[1];

        if (minKey.isEmpty()) {
            errmsg = "no min key specified";
            return false;
        }

        if (maxKey.isEmpty()) {
            errmsg = "no max key specified";
            return false;
        }

        // Epoch is optional, and if not set indicates we should use the latest epoch
        OID epoch;
        if (!FieldParser::extract(cmdObj, epochField, &epoch, &errmsg)) {
            return false;
        }

        mergeChunks(opCtx, nss, minKey, maxKey, epoch);
        return true;
    }

} mergeChunksCmd;

BSONField<std::string> MergeChunksCommand::nsField("mergeChunks");
BSONField<std::vector<BSONObj>> MergeChunksCommand::boundsField("bounds");
BSONField<OID> MergeChunksCommand::epochField("epoch");

}  // namespace
}  // namespace mongo
