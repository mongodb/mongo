// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

std::vector<MetadataInconsistencyItem> getHiddenCollectionsInconsistencies(
    OperationContext* opCtx) {
    static const auto rawPipelineStages = [] {
        auto rawPipelineBSON = fromjson(R"({pipeline: [
            {
                $addFields: {
                    dbName: {
                        $arrayElemAt: [{
                            $split: ['$_id', '.']
                        }, 0]
                    }
                }
            },
            {
                $match: {
                    dbName: {
                        $ne: 'config'
                    }
                }
            },
            {
                $lookup: {
                    from: 'databases',
                    localField: 'dbName',
                    foreignField: '_id',
                    as: 'db'
                }
            },
            {
                $match: {
                    db: []
                }
            }
        ]})");
        return parsePipelineFromBSON(rawPipelineBSON.firstElement());
    }();

    AggregateCommandRequest hiddenCollAggRequest{NamespaceString::kConfigsvrCollectionsNamespace,
                                                 rawPipelineStages};
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    auto rawHiddenColls = catalogClient->runCatalogAggregation(
        opCtx, hiddenCollAggRequest, {repl::ReadConcernLevel::kSnapshotReadConcern});

    std::vector<MetadataInconsistencyItem> inconsistencies;
    inconsistencies.reserve(rawHiddenColls.size());
    for (auto&& rawHiddenColl : rawHiddenColls) {
        CollectionType coll{rawHiddenColl};
        inconsistencies.emplace_back(metadata_consistency_util::makeInconsistency(
            MetadataInconsistencyTypeEnum::kHiddenShardedCollection,
            HiddenShardedCollectionDetails{coll.getNss(), coll.toBSON()}));
    }
    return inconsistencies;
}

class ConfigsvrCheckClusterMetadataConsistencyCommand final
    : public TypedCommand<ConfigsvrCheckClusterMetadataConsistencyCommand> {
public:
    using Request = ConfigsvrCheckClusterMetadataConsistency;
    using Response = CursorInitialReply;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << Request::kCommandName
                                  << " can only be run on the config server",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            std::vector<MetadataInconsistencyItem> inconsistencies;

            auto hiddenCollectionsIncon = getHiddenCollectionsInconsistencies(opCtx);
            inconsistencies.insert(inconsistencies.end(),
                                   std::make_move_iterator(hiddenCollectionsIncon.begin()),
                                   std::make_move_iterator(hiddenCollectionsIncon.end()));

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx, ns(), std::move(inconsistencies), request().getCursor(), request().toBSON());
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString{request().getDbName()};
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(ConfigsvrCheckClusterMetadataConsistencyCommand).forShard();

}  // namespace
}  // namespace mongo
