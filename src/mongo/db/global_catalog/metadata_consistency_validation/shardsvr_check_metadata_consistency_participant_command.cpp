/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrCheckMetadataConsistencyParticipantCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencyParticipantCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistencyParticipant;
    using Response = CursorInitialReply;

    bool adminOnly() const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            ShardingState::get(opCtx)->assertCanAcceptShardedCommands();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto nss = ns();
            const auto shardId = ShardingState::get(opCtx)->shardId();
            const auto& primaryShardId = request().getPrimaryShardId();
            const auto commandLevel = metadata_consistency_util::getCommandLevel(nss);

            tassert(1011703,
                    str::stream()
                        << "Unexpected parameter during the internal execution of "
                           "checkMetadataConsistency command. The shard server was expecting to "
                           "receive a database or collection level parameter, but received "
                        << MetadataConsistencyCommandLevel_serializer(commandLevel)
                        << " with namespace " << nss.toStringForErrorMsg(),
                    commandLevel == MetadataConsistencyCommandLevelEnum::kCollectionLevel ||
                        commandLevel == MetadataConsistencyCommandLevelEnum::kDatabaseLevel);

            // Get the list of collections from configsvr sorted by namespace
            const auto configsvrCollections =
                getCollectionsListFromConfigServer(opCtx, nss, commandLevel);

            const auto checkRangeDeletionIndexes =
                request().getCommonFields().getCheckRangeDeletionIndexes();
            uassert(ErrorCodes::InvalidOptions,
                    "Range deletion missing shard key index inconsistency check is not supported "
                    "with the current FCV. Upgrade to the highest FCV for performing the check.",
                    !checkRangeDeletionIndexes ||
                        feature_flags::gCheckRangeDeletionsWithMissingShardKeyIndex.isEnabled(
                            VersionContext::getDecoration(opCtx),
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

            auto inconsistencies = checkCollectionMetadataConsistency(opCtx,
                                                                      nss,
                                                                      commandLevel,
                                                                      shardId,
                                                                      primaryShardId,
                                                                      configsvrCollections,
                                                                      checkRangeDeletionIndexes);

            // If this is the primary shard of the db coordinate index check across shards
            const auto optionalCheckIndexes = request().getCommonFields().getCheckIndexes();
            if (shardId == primaryShardId) {
                if (optionalCheckIndexes) {
                    auto indexInconsistencies =
                        metadata_consistency_util::checkIndexesConsistencyAcrossShards(
                            opCtx, configsvrCollections);
                    inconsistencies.insert(inconsistencies.end(),
                                           std::make_move_iterator(indexInconsistencies.begin()),
                                           std::make_move_iterator(indexInconsistencies.end()));
                }

                if (feature_flags::gShardAuthoritativeDbMetadataCRUD.isEnabled(
                        VersionContext::getDecoration(opCtx),
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                    !nss.isConfigDB()) {
                    const auto dbInGlobalCatalog =
                        getDatabaseMetadataFromConfigServer(opCtx, nss.dbName());

                    auto dbMetadataInconsistencies =
                        metadata_consistency_util::checkDatabaseMetadataConsistency(
                            opCtx, dbInGlobalCatalog);
                    inconsistencies.insert(
                        inconsistencies.end(),
                        std::make_move_iterator(dbMetadataInconsistencies.begin()),
                        std::make_move_iterator(dbMetadataInconsistencies.end()));
                }

                auto collMetadataInconsistencies =
                    metadata_consistency_util::checkCollectionMetadataConsistencyAcrossShards(
                        opCtx, configsvrCollections);
                inconsistencies.insert(inconsistencies.end(),
                                       std::make_move_iterator(collMetadataInconsistencies.begin()),
                                       std::make_move_iterator(collMetadataInconsistencies.end()));
            }

            auto exec = metadata_consistency_util::makeQueuedPlanExecutor(
                opCtx, std::move(inconsistencies), nss);

            ClientCursorParams cursorParams{
                std::move(exec),
                nss,
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                APIParameters::get(opCtx),
                opCtx->getWriteConcern(),
                repl::ReadConcernArgs::get(opCtx),
                ReadPreferenceSetting::get(opCtx),
                request().toBSON(),
                {Privilege(ResourcePattern::forClusterResource(nss.tenantId()),
                           ActionType::internal)}};

            const auto batchSize = [&]() -> long long {
                const auto& cursorOpts = request().getCursor();
                if (cursorOpts && cursorOpts->getBatchSize()) {
                    return *cursorOpts->getBatchSize();
                } else {
                    return query_request_helper::getDefaultBatchSize();
                }
            }();

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx, std::move(cursorParams), batchSize);
        }

    private:
        std::vector<CollectionType> getCollectionsListFromConfigServer(
            OperationContext* opCtx,
            const NamespaceString& nss,
            const MetadataConsistencyCommandLevelEnum& commandLevel) {
            switch (commandLevel) {
                case MetadataConsistencyCommandLevelEnum::kDatabaseLevel: {
                    return Grid::get(opCtx)->catalogClient()->getCollections(
                        opCtx,
                        nss.dbName(),
                        repl::ReadConcernLevel::kMajorityReadConcern,
                        BSON(CollectionType::kNssFieldName << 1) /*sort*/);
                }
                case MetadataConsistencyCommandLevelEnum::kCollectionLevel: {
                    try {
                        auto collectionType =
                            Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
                        return {std::move(collectionType)};
                    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                        // If we don't find the nss, it means that the collection is not sharded.
                        return {};
                    }
                }
                default:
                    tasserted(
                        1011704,
                        str::stream()
                            << "Unexpected parameter during the internal execution of "
                               "checkMetadataConsistency command. The shard server was expecting "
                               "to receive a database or collection level parameter, but received "
                            << MetadataConsistencyCommandLevel_serializer(commandLevel)
                            << " with namespace " << nss.toStringForErrorMsg());
            }
        }

        DatabaseType getDatabaseMetadataFromConfigServer(OperationContext* opCtx,
                                                         const DatabaseName& dbName) {
            return Grid::get(opCtx)->catalogClient()->getDatabase(
                opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);
        }

        std::vector<MetadataInconsistencyItem> checkCollectionMetadataConsistency(
            OperationContext* opCtx,
            const NamespaceString& nss,
            const MetadataConsistencyCommandLevelEnum& commandLevel,
            const ShardId& shardId,
            const ShardId& primaryShardId,
            const std::vector<mongo::CollectionType>& shardingCatalogCollections,
            const bool checkRangeDeletionIndexes) {
            std::vector<CollectionPtr> localCatalogCollections;
            auto collCatalogSnapshot = [&] {
                switch (commandLevel) {
                    case MetadataConsistencyCommandLevelEnum::kDatabaseLevel: {
                        auto collCatalogSnapshot = [&] {
                            // Lock db in mode IS while taking the collection catalog snapshot to
                            // ensure that we serialize with non-atomic collection and index
                            // creation performed by the MigrationDestinationManager. Without this
                            // lock we could potentially acquire a snapshot in which a collection
                            // have been already created by the MigrationDestinationManager but the
                            // relative shardkey index is still missing.
                            AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IS);
                            return CollectionCatalog::get(opCtx);
                        }();

                        for (auto&& coll : collCatalogSnapshot->range(nss.dbName())) {
                            if (!coll) {
                                continue;
                            }
                            // The collection catalog snapshot will be in scope until the end of
                            // the command execution, so we can safely use CollectionPtr_UNSAFE as
                            // the instance pointed by Collection* will stay in scope as a
                            // consequence.
                            localCatalogCollections.emplace_back(
                                CollectionPtr::CollectionPtr_UNSAFE(coll));
                        }
                        std::sort(localCatalogCollections.begin(),
                                  localCatalogCollections.end(),
                                  [](const CollectionPtr& prev, const CollectionPtr& next) {
                                      return prev->ns() < next->ns();
                                  });

                        return collCatalogSnapshot;
                    }
                    case MetadataConsistencyCommandLevelEnum::kCollectionLevel: {
                        auto collCatalogSnapshot = [&] {
                            // Lock collection in mode IS while taking the collection catalog
                            // snapshot to ensure that we serialize with non-atomic collection and
                            // index creation performed by the MigrationDestinationManager. Without
                            // this lock we could potentially acquire a snapshot in which a
                            // collection have been already created by the
                            // MigrationDestinationManager but the relative shardkey index is still
                            // missing.
                            AutoGetCollection coll(
                                opCtx,
                                nss,
                                MODE_IS,
                                AutoGetCollection::Options{}.viewMode(
                                    auto_get_collection::ViewMode::kViewsPermitted));
                            return CollectionCatalog::get(opCtx);
                        }();

                        // The collection catalog snapshot will be in scope until the end of
                        // the command execution, so we can safely use CollectionPtr_UNSAFE as
                        // the instance pointed by Collection* will stay in scope as a
                        // consequence.
                        if (auto coll =
                                collCatalogSnapshot->lookupCollectionByNamespace(opCtx, nss)) {
                            localCatalogCollections.emplace_back(
                                CollectionPtr::CollectionPtr_UNSAFE(coll));
                        }

                        return collCatalogSnapshot;
                    }
                    default:
                        tasserted(1011705,
                                  str::stream()
                                      << "Unexpected parameter during the internal execution of "
                                         "checkMetadataConsistency command. The shard server was "
                                         "expecting "
                                         "to receive a database or collection level parameter, but "
                                         "received "
                                      << MetadataConsistencyCommandLevel_serializer(commandLevel)
                                      << " with namespace " << nss.toStringForErrorMsg());
                }
            }();

            // Check consistency between local metadata and configsvr metadata
            return metadata_consistency_util::checkCollectionMetadataConsistency(
                opCtx,
                shardId,
                primaryShardId,
                shardingCatalogCollections,
                localCatalogCollections,
                checkRangeDeletionIndexes);
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
MONGO_REGISTER_COMMAND(ShardsvrCheckMetadataConsistencyParticipantCommand).forShard();

}  // namespace
}  // namespace mongo
