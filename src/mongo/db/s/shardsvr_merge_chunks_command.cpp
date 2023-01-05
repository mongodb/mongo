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

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/merge_chunk_request_gen.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

Shard::CommandResponse commitMergeOnConfigServer(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const OID& epoch,
                                                 const boost::optional<Timestamp>& timestamp,
                                                 const ChunkRange& chunkRange,
                                                 const CollectionMetadata& metadata) {
    auto const shardingState = ShardingState::get(opCtx);
    const auto currentTime = VectorClock::get(opCtx)->getTime();

    ConfigSvrMergeChunks request{nss, shardingState->shardId(), metadata.getUUID(), chunkRange};
    request.setValidAfter(currentTime.clusterTime().asTimestamp());
    request.setEpoch(epoch);
    request.setTimestamp(timestamp);

    auto cmdResponse =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            NamespaceString::kAdminDb.toString(),
            request.toBSON(BSON(WriteConcernOptions::kWriteConcernField
                                << ShardingCatalogClient::kMajorityWriteConcern.toBSON())),
            Shard::RetryPolicy::kIdempotent));

    return cmdResponse;
}

void mergeChunks(OperationContext* opCtx,
                 const NamespaceString& nss,
                 const BSONObj& minKey,
                 const BSONObj& maxKey,
                 const OID& expectedEpoch,
                 const boost::optional<Timestamp>& expectedTimestamp) {
    auto scopedSplitOrMergeChunk(
        uassertStatusOK(ActiveMigrationsRegistry::get(opCtx).registerSplitOrMergeChunk(
            opCtx, nss, ChunkRange(minKey, maxKey))));

    auto& oss = OperationShardingState::get(opCtx);
    if (!oss.getShardVersion(nss)) {
        onShardVersionMismatch(opCtx, nss, boost::none);
    }

    const auto metadataBeforeMerge = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        auto csr = CollectionShardingRuntime::get(opCtx, nss);

        // If there is a version attached to the OperationContext, validate it
        if (oss.getShardVersion(nss)) {
            csr->checkShardVersionOrThrow(opCtx);
        } else {
            auto optMetadata = csr->getCurrentMetadataIfKnown();

            ShardId shardId = ShardingState::get(opCtx)->shardId();

            uassert(StaleConfigInfo(nss,
                                    ChunkVersion::IGNORED() /* receivedVersion */,
                                    boost::none /* wantedVersion */,
                                    shardId),
                    str::stream() << "Collection " << nss.ns() << " needs to be recovered",
                    optMetadata);
            uassert(StaleConfigInfo(nss,
                                    ChunkVersion::IGNORED() /* receivedVersion */,
                                    ChunkVersion::UNSHARDED() /* wantedVersion */,
                                    shardId),
                    str::stream() << "Collection " << nss.ns() << " is not sharded",
                    optMetadata->isSharded());
            const auto epoch = optMetadata->getShardVersion().epoch();
            uassert(StaleConfigInfo(nss,
                                    ChunkVersion::IGNORED() /* receivedVersion */,
                                    optMetadata->getShardVersion() /* wantedVersion */,
                                    shardId),
                    str::stream() << "Could not merge chunks because collection " << nss.ns()
                                  << " has changed since merge was sent (sent epoch: "
                                  << expectedEpoch << ", current epoch: " << epoch << ")",
                    epoch == expectedEpoch &&
                        (!expectedTimestamp ||
                         optMetadata->getShardVersion().getTimestamp() == expectedTimestamp));
        }

        return *csr->getCurrentMetadataIfKnown();
    }();

    ChunkRange chunkRange(minKey, maxKey);

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, the range " << chunkRange.toString()
                          << " is not valid for collection " << nss.ns() << " with key pattern "
                          << metadataBeforeMerge.getKeyPattern().toString(),
            metadataBeforeMerge.isValidKey(minKey) && metadataBeforeMerge.isValidKey(maxKey));

    auto cmdResponse = commitMergeOnConfigServer(
        opCtx, nss, expectedEpoch, expectedTimestamp, chunkRange, metadataBeforeMerge);

    auto shardVersionReceived = [&]() -> boost::optional<ChunkVersion> {
        // Old versions might not have the shardVersion field
        if (cmdResponse.response[ChunkVersion::kShardVersionField]) {
            return ChunkVersion::fromBSONPositionalOrNewerFormat(
                cmdResponse.response[ChunkVersion::kShardVersionField]);
        }
        return boost::none;
    }();
    onShardVersionMismatch(opCtx, nss, std::move(shardVersionReceived));

    uassertStatusOKWithContext(cmdResponse.commandStatus, "Failed to commit chunk merge");
    uassertStatusOKWithContext(cmdResponse.writeConcernStatus, "Failed to commit chunk merge");
}

class MergeChunksCommand : public ErrmsgCommandDeprecated {
public:
    MergeChunksCommand() : ErrmsgCommandDeprecated("mergeChunks", "_shardsvrMergeChunks") {}

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

    static BSONField<std::string> nsField;
    static BSONField<std::vector<BSONObj>> boundsField;
    static BSONField<OID> epochField;
    static BSONField<Timestamp> timestampField;

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

        OID epoch;
        if (!FieldParser::extract(cmdObj, epochField, &epoch, &errmsg)) {
            return false;
        }

        boost::optional<Timestamp> timestamp;
        if (cmdObj[timestampField()]) {
            timestamp.emplace();
            if (!FieldParser::extract(cmdObj, timestampField, timestamp.get_ptr(), &errmsg)) {
                return false;
            }
        }

        mergeChunks(opCtx, nss, minKey, maxKey, epoch, timestamp);
        return true;
    }

} mergeChunksCmd;

BSONField<std::string> MergeChunksCommand::nsField("mergeChunks");
BSONField<std::vector<BSONObj>> MergeChunksCommand::boundsField("bounds");
BSONField<OID> MergeChunksCommand::epochField("epoch");
BSONField<Timestamp> MergeChunksCommand::timestampField("timestamp");

}  // namespace
}  // namespace mongo
