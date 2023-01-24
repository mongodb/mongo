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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class ShardsvrCheckMetadataConsistencyParticipantCommand final
    : public TypedCommand<ShardsvrCheckMetadataConsistencyParticipantCommand> {
public:
    using Request = ShardsvrCheckMetadataConsistencyParticipant;
    using Response = MetadataInconsistencies;

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
            uassertStatusOK(ShardingState::get(opCtx)->canAcceptShardedCommands());

            const auto& nss = ns();
            const auto& shardId = ShardingState::get(opCtx)->shardId();
            const auto& collectionCatalog = CollectionCatalog::get(opCtx);
            const auto& primaryShardId = request().getPrimaryShardId();

            // Get the list of collections from configsvr sorted by namespace
            auto catalogClientCollections = Grid::get(opCtx)->catalogClient()->getCollections(
                opCtx,
                nss.db(),
                repl::ReadConcernLevel::kMajorityReadConcern,
                BSON(CollectionType::kNssFieldName << 1) /*sort*/);

            // Get the list of local collections sorted by namespace
            Lock::DBLock dbLock(opCtx, nss.db(), MODE_S);
            auto localNssCollections =
                collectionCatalog->getAllCollectionNamesFromDb(opCtx, nss.db());
            std::sort(localNssCollections.begin(), localNssCollections.end());
            std::vector<CollectionPtr> localCollection;
            for (const auto& localNss : localNssCollections) {
                localCollection.push_back(
                    collectionCatalog->lookupCollectionByNamespace(opCtx, localNss));
            }

            // Check consistency between local metadata and configsvr metadata
            Response response;
            auto inconsistencies =
                metadata_consistency_util::checkCollectionMetadataInconsistencies(
                    opCtx, shardId, primaryShardId, catalogClientCollections, localCollection);
            response.setInconsistencies(std::move(inconsistencies));
            return response;
        }

    private:
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
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrCheckMetadataConsistencyParticipantCommand;

}  // namespace
}  // namespace mongo
