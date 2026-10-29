// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
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
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
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

class ConfigsvrCheckMetadataConsistencyCommand final
    : public TypedCommand<ConfigsvrCheckMetadataConsistencyCommand> {
public:
    using Request = ConfigsvrCheckMetadataConsistency;
    using Response = CursorInitialReply;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool adminOnly() const override {
        return false;
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

            const auto nss = ns();

            std::vector<MetadataInconsistencyItem> inconsistenciesMerged;
            const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

            const auto commandLevel = metadata_consistency_util::getCommandLevel(nss);
            switch (commandLevel) {
                case MetadataConsistencyCommandLevelEnum::kDatabaseLevel: {
                    const auto collections = catalogClient->getCollections(opCtx, nss.dbName());

                    for (const auto& coll : collections) {
                        _runChecksForCollection(opCtx, coll, inconsistenciesMerged);
                    }
                    break;
                }
                case MetadataConsistencyCommandLevelEnum::kCollectionLevel: {
                    try {
                        const auto coll = catalogClient->getCollection(opCtx, nss);
                        _runChecksForCollection(opCtx, coll, inconsistenciesMerged);
                    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                        // If we don't find the nss, it means that the collection is not sharded.
                    }
                    break;
                }
                default:
                    tasserted(1011702,
                              str::stream()
                                  << "Unexpected parameter during the internal execution of "
                                     "checkMetadataConsistency command. The config server was "
                                     "expecting to receive a database or collection level "
                                     "parameter, but received "
                                  << idl::serialize(commandLevel) << " with namespace "
                                  << nss.toStringForErrorMsg());
            }

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx,
                nss,
                std::move(inconsistenciesMerged),
                request().getCursor(),
                request().toBSON());
        }

    private:
        void _runChecksForCollection(
            OperationContext* opCtx,
            const CollectionType& coll,
            std::vector<MetadataInconsistencyItem>& inconsistenciesMerged) {
            auto chunksInconsistencies =
                metadata_consistency_util::checkChunksConsistency(opCtx, coll);

            inconsistenciesMerged.insert(inconsistenciesMerged.end(),
                                         std::make_move_iterator(chunksInconsistencies.begin()),
                                         std::make_move_iterator(chunksInconsistencies.end()));

            auto collectionsInconsistencies =
                metadata_consistency_util::checkCollectionShardingMetadataConsistency(opCtx, coll);

            inconsistenciesMerged.insert(
                inconsistenciesMerged.end(),
                std::make_move_iterator(collectionsInconsistencies.begin()),
                std::make_move_iterator(collectionsInconsistencies.end()));

            auto zonesInconsistencies = metadata_consistency_util::checkZonesConsistency(
                opCtx, coll, _getCollectionZones(opCtx, coll.getNss()));

            inconsistenciesMerged.insert(inconsistenciesMerged.end(),
                                         std::make_move_iterator(zonesInconsistencies.begin()),
                                         std::make_move_iterator(zonesInconsistencies.end()));
        }

        std::vector<TagsType> _getCollectionZones(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
            const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
            return uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));
        }

        NamespaceString ns() const override {
            return request().getNamespace();
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
MONGO_REGISTER_COMMAND(ConfigsvrCheckMetadataConsistencyCommand).forShard();

}  // namespace
}  // namespace mongo
