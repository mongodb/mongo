// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/catalog_cache_diagnostics_helpers.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

void appendFilteringMetadataCacheInfo(OperationContext* opCtx,
                                      BSONObjBuilder* builder,
                                      const NamespaceString& nss,
                                      bool fullMetadata) {
    builder->append(
        "configServer",
        Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString().toString());

    AutoGetCollection autoColl(
        opCtx,
        nss,
        MODE_IS,
        auto_get_collection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));
    const auto scopedCsr =
        CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);

    auto optMetadata = scopedCsr->getCurrentMetadataIfKnown();
    if (!optMetadata) {
        catalog_cache_diagnostics_helpers::appendWhenUnknown(builder, fullMetadata);
    } else {
        const auto& metadata = *optMetadata;
        builder->appendTimestamp("global", metadata.getShardPlacementVersion().toLong());

        if (fullMetadata) {
            BSONObjBuilder metadataBuilder(builder->subobjStart("metadata"));
            if (metadata.isSharded()) {
                metadataBuilder.appendTimestamp("collVersion",
                                                metadata.getCollPlacementVersion().toLong());
                metadataBuilder.append("collVersionEpoch",
                                       metadata.getCollPlacementVersion().epoch());
                metadataBuilder.append("collVersionTimestamp",
                                       metadata.getCollPlacementVersion().getTimestamp());

                metadataBuilder.appendTimestamp(
                    "shardVersion", metadata.getShardPlacementVersionForLogging().toLong());
                metadataBuilder.append("shardVersionEpoch",
                                       metadata.getShardPlacementVersionForLogging().epoch());
                metadataBuilder.append(
                    "shardVersionTimestamp",
                    metadata.getShardPlacementVersionForLogging().getTimestamp());

                metadataBuilder.append("keyPattern", metadata.getShardKeyPattern().toBSON());

                BSONArrayBuilder chunksArr(metadataBuilder.subarrayStart("chunks"));
                metadata.toBSONChunks(&chunksArr);
                chunksArr.doneFast();
            }
            metadataBuilder.doneFast();
        }
    }
}

class GetShardVersion : public BasicCommand {
public:
    GetShardVersion() : BasicCommand("getShardVersion") {}

    std::string help() const override {
        return " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forExactNamespace(parseNs(dbName, cmdObj)),
                     ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
        bool fullMetadata = cmdObj["fullMetadata"].trueValue();

        // On shard servers we can dump either the routing info or the filtering info, controlled
        // via the "latestCached" argument.
        if (cmdObj["latestCached"].trueValue()) {
            catalog_cache_diagnostics_helpers::appendLatestCachedCollInfo(
                opCtx, &result, nss, fullMetadata);
        } else {
            appendFilteringMetadataCacheInfo(opCtx, &result, nss, fullMetadata);
        }

        return true;
    }
};
MONGO_REGISTER_COMMAND(GetShardVersion).forShard();

}  // namespace
}  // namespace mongo
