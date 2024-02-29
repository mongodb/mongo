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


#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/chunk_operation_precondition_checks.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/merge_chunk_request_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/namespace_string_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

Shard::CommandResponse commitMergeOnConfigServer(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const OID& epoch,
                                                 const boost::optional<Timestamp>& timestamp,
                                                 const ChunkRange& chunkRange,
                                                 const CollectionMetadata& metadata) {
    auto const shardingState = ShardingState::get(opCtx);

    ConfigSvrMergeChunks request{nss, shardingState->shardId(), metadata.getUUID(), chunkRange};
    request.setEpoch(epoch);
    request.setTimestamp(timestamp);

    auto cmdResponse =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
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

    ChunkRange chunkRange(minKey, maxKey);

    // Check that the preconditions for merge chunks are met and throw StaleShardVersion otherwise.
    const auto metadataBeforeMerge = [&]() {
        onCollectionPlacementVersionMismatch(opCtx, nss, boost::none);
        const auto [metadata, indexInfo] =
            checkCollectionIdentity(opCtx, nss, expectedEpoch, expectedTimestamp);
        checkShardKeyPattern(opCtx, nss, metadata, indexInfo, chunkRange);
        checkRangeOwnership(opCtx, nss, metadata, indexInfo, chunkRange);
        return metadata;
    }();

    auto cmdResponse = commitMergeOnConfigServer(
        opCtx, nss, expectedEpoch, expectedTimestamp, chunkRange, metadataBeforeMerge);

    auto chunkVersionReceived = [&]() -> boost::optional<ChunkVersion> {
        // Old versions might not have the shardVersion field
        if (cmdResponse.response[ChunkVersion::kChunkVersionField]) {
            return ChunkVersion::parse(cmdResponse.response[ChunkVersion::kChunkVersionField]);
        }
        return boost::none;
    }();
    onCollectionPlacementVersionMismatch(opCtx, nss, std::move(chunkVersionReceived));

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

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forClusterResource(dbName.tenantId()),
                     ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
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
                   const DatabaseName& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        ShardingState::get(opCtx)->assertCanAcceptShardedCommands();

        const NamespaceString nss(parseNs(dbName, cmdObj));

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

        if (SimpleBSONObjComparator::kInstance.evaluate(minKey >= maxKey)) {
            errmsg = "the specified max bound must be greater than the specified min bound";
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
};
MONGO_REGISTER_COMMAND(MergeChunksCommand).forShard();

BSONField<std::string> MergeChunksCommand::nsField("mergeChunks");
BSONField<std::vector<BSONObj>> MergeChunksCommand::boundsField("bounds");
BSONField<OID> MergeChunksCommand::epochField("epoch");
BSONField<Timestamp> MergeChunksCommand::timestampField("timestamp");

}  // namespace
}  // namespace mongo
