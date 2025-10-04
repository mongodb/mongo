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

#include "mongo/db/global_catalog/ddl/split_chunk.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/global_catalog/ddl/split_chunk_request_type.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <iterator>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

// This shard version is used as the received version in StaleConfigInfo since we do not have
// information about the received version of the operation.
ShardVersion ShardVersionPlacementIgnored() {
    return ShardVersionFactory::make(ChunkVersion::IGNORED());
}

/**
 * Checks the collection's metadata for a successful split on the specified chunkRange using the
 * specified split points. Returns false if the metadata's chunks don't match the new chunk
 * boundaries exactly.
 */
bool checkMetadataForSuccessfulSplitChunk(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const OID& expectedEpoch,
                                          const boost::optional<Timestamp>& expectedTimestamp,
                                          const ChunkRange& chunkRange,
                                          const std::vector<BSONObj>& splitPoints) {
    const auto scopedCSR = CollectionShardingRuntime::acquireShared(opCtx, nss);
    const auto metadataAfterSplit = scopedCSR->getCurrentMetadataIfKnown();

    ShardId shardId = ShardingState::get(opCtx)->shardId();

    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            boost::none /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " needs to be recovered",
            metadataAfterSplit);
    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            ShardVersion::UNSHARDED() /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " is not sharded",
            metadataAfterSplit->isSharded());
    const auto placementVersion = metadataAfterSplit->getShardPlacementVersion();
    const auto epoch = placementVersion.epoch();
    uassert(StaleConfigInfo(nss,
                            ShardVersionPlacementIgnored() /* receivedVersion */,
                            ShardVersionFactory::make(*metadataAfterSplit) /* wantedVersion */,
                            shardId),
            str::stream() << "Collection " << nss.toStringForErrorMsg()
                          << " changed since split start",
            epoch == expectedEpoch &&
                (!expectedTimestamp || placementVersion.getTimestamp() == expectedTimestamp));

    ChunkType nextChunk;
    for (auto it = splitPoints.begin(); it != splitPoints.end(); ++it) {
        // Check that all new chunks fit the new chunk boundaries
        const auto& currentChunkMinKey =
            it == splitPoints.begin() ? chunkRange.getMin() : *std::prev(it);
        const auto& currentChunkMaxKey = *it;
        if (!metadataAfterSplit->getNextChunk(currentChunkMinKey, &nextChunk) ||
            nextChunk.getMax().woCompare(currentChunkMaxKey)) {
            return false;
        }
    }
    // Special check for the last chunk produced.
    if (!metadataAfterSplit->getNextChunk(splitPoints.back(), &nextChunk) ||
        nextChunk.getMax().woCompare(chunkRange.getMax())) {
        return false;
    }

    return true;
}

}  // namespace

Status splitChunk(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const BSONObj& keyPatternObj,
                  const ChunkRange& chunkRange,
                  std::vector<BSONObj>&& splitPoints,
                  const std::string& shardName,
                  const OID& expectedCollectionEpoch,
                  const boost::optional<Timestamp>& expectedCollectionTimestamp) {
    auto scopedSplitOrMergeChunk(uassertStatusOK(
        ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(opCtx, nss, chunkRange)));

    // If the shard key is hashed, then we must make sure that the split points are of supported
    // data types.
    const auto hashedField = ShardKeyPattern::extractHashedField(keyPatternObj);
    if (hashedField) {
        for (const auto& splitPoint : splitPoints) {
            auto hashedSplitElement = splitPoint[hashedField.fieldName()];
            if (!ShardKeyPattern::isValidHashedValue(hashedSplitElement)) {
                return {ErrorCodes::CannotSplit,
                        str::stream() << "splitChunk cannot split chunk " << chunkRange.toString()
                                      << ", split point " << hashedSplitElement.toString()
                                      << "Value of type '" << hashedSplitElement.type()
                                      << "' is not allowed for hashed fields"};
            }
        }
    }

    // Commit the split to the config server.
    auto request = SplitChunkRequest(nss,
                                     shardName,
                                     expectedCollectionEpoch,
                                     expectedCollectionTimestamp,
                                     chunkRange,
                                     std::move(splitPoints));

    auto configCmdObj = request.toConfigCommandBSON(defaultMajorityWriteConcernDoNotUse().toBSON());

    auto cmdResponseStatus = Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
        opCtx,
        kPrimaryOnlyReadPreference,
        DatabaseName::kAdmin,
        configCmdObj,
        Shard::RetryPolicy::kIdempotent);

    // If we failed to get any response from the config server at all, despite retries, then we
    // should just go ahead and fail the whole operation.
    if (!cmdResponseStatus.isOK()) {
        return cmdResponseStatus.getStatus();
    }

    const Shard::CommandResponse& cmdResponse = cmdResponseStatus.getValue();

    boost::optional<ChunkVersion> chunkVersionReceived = [&]() -> boost::optional<ChunkVersion> {
        // old versions might not have the shardVersion field
        if (cmdResponse.response[ChunkVersion::kChunkVersionField]) {
            return ChunkVersion::parse(cmdResponse.response[ChunkVersion::kChunkVersionField]);
        }
        return boost::none;
    }();
    uassertStatusOK(FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
        opCtx, nss, chunkVersionReceived));

    // Check commandStatus and writeConcernStatus
    auto commandStatus = cmdResponse.commandStatus;
    auto writeConcernStatus = cmdResponse.writeConcernStatus;

    // Send stale epoch if epoch of request did not match epoch of collection
    if (commandStatus == ErrorCodes::StaleEpoch) {
        return commandStatus;
    }

    // If _configsvrCommitChunkSplit returned an error, look at the metadata to
    // determine if the split actually did happen. This can happen if there's a network error
    // getting the response from the first call to _configsvrCommitChunkSplit, but it actually
    // succeeds, thus the automatic retry fails with a precondition violation, for example.
    if (!commandStatus.isOK() || !writeConcernStatus.isOK()) {
        if (checkMetadataForSuccessfulSplitChunk(opCtx,
                                                 nss,
                                                 expectedCollectionEpoch,
                                                 expectedCollectionTimestamp,
                                                 chunkRange,
                                                 request.getSplitPoints())) {
            // Split was committed.
        } else if (!commandStatus.isOK()) {
            return commandStatus;
        } else if (!writeConcernStatus.isOK()) {
            return writeConcernStatus;
        }
    }

    return Status::OK();
}

}  // namespace mongo
