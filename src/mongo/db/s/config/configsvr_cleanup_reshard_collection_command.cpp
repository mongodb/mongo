/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/resharding_manual_cleanup.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/cleanup_reshard_collection_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/namespace_string_util.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

auto constructFinalMetadataRemovalUpdateOperation(OperationContext* opCtx,
                                                  const NamespaceString& nss) {
    auto query = BSON(CollectionType::kNssFieldName
                      << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));

    auto collEntryFieldsToUnset = BSON(CollectionType::kReshardingFieldsFieldName
                                       << 1 << CollectionType::kAllowMigrationsFieldName << 1);
    auto collEntryFieldsToUpdate =
        BSON(CollectionType::kUpdatedAtFieldName
             << opCtx->getServiceContext()->getPreciseClockSource()->now());

    auto update = BSON("$unset" << collEntryFieldsToUnset << "$set" << collEntryFieldsToUpdate);

    return BatchedCommandRequest::buildUpdateOp(CollectionType::ConfigNS,
                                                query,
                                                update,
                                                false,  // upsert
                                                false   // multi
    );
}

class ConfigsvrCleanupReshardCollectionCommand final
    : public TypedCommand<ConfigsvrCleanupReshardCollectionCommand> {
public:
    using Request = ConfigsvrCleanupReshardCollection;

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "_configsvrCleanupReshardCollection can only be run on config servers",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            repl::ReadConcernArgs::get(opCtx) =
                repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

            const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
            auto collEntry = catalogClient->getCollection(opCtx, ns());
            if (!collEntry.getReshardingFields()) {
                // If the collection entry doesn't have resharding fields, we assume that the
                // resharding operation has already been cleaned up.
                return;
            }

            ReshardingCoordinatorCleaner cleaner(
                ns(), collEntry.getReshardingFields()->getReshardingUUID());
            cleaner.clean(opCtx);

            ShardingCatalogManager::get(opCtx)
                ->bumpCollectionPlacementVersionAndChangeMetadataInTxn(
                    opCtx, ns(), [&](OperationContext* opCtx, TxnNumber txnNumber) {
                        auto update = constructFinalMetadataRemovalUpdateOperation(opCtx, ns());
                        auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
                            opCtx, CollectionType::ConfigNS, update, txnNumber);
                    });

            collEntry = catalogClient->getCollection(opCtx, ns());

            uassert(
                5403504,
                fmt::format(
                    "Expected collection entry for {} to no longer have resharding metadata, but "
                    "metadata documents still exist; please rerun the cleanupReshardCollection "
                    "command",
                    ns().toStringForErrorMsg()),
                !collEntry.getReshardingFields());
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
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

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Aborts and cleans up any in-progress resharding operations for this "
               "collection.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(ConfigsvrCleanupReshardCollectionCommand).forShard();

}  // namespace
}  // namespace mongo
