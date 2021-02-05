/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/shard_collection_legacy.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"

namespace mongo {
namespace {

void inferCollationFromLocalCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardsvrCreateCollection& request,
    ShardsvrShardCollectionRequest* shardsvrShardCollectionRequest) {
    auto& collation = request.getCollation().value();
    auto collator = uassertStatusOK(
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
    uassert(ErrorCodes::BadValue,
            str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                          << "but found: " << collation,
            !collator);

    BSONObj res, defaultCollation, collectionOptions;
    DBDirectClient client(opCtx);

    auto allRes = client.getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));

    if (!allRes.empty())
        res = allRes.front().getOwned();

    if (!res.isEmpty() && res["options"].type() == BSONType::Object) {
        collectionOptions = res["options"].Obj();
    }

    // Get collection default collation.
    BSONElement collationElement;
    auto status =
        bsonExtractTypedField(collectionOptions, "collation", BSONType::Object, &collationElement);
    if (status.isOK()) {
        defaultCollation = collationElement.Obj().getOwned();
        uassert(ErrorCodes::BadValue,
                "Default collation in collection metadata cannot be empty.",
                !defaultCollation.isEmpty());
    } else if (status != ErrorCodes::NoSuchKey) {
        uassertStatusOK(status);
    }

    shardsvrShardCollectionRequest->setCollation(defaultCollation.getOwned());
}

CreateCollectionResponse createCollectionLegacy(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const ShardsvrCreateCollection& request) {
    const auto dbInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, nss.db()));

    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "sharding not enabled for db " << nss.db(),
            dbInfo.shardingEnabled());

    if (nss.db() == NamespaceString::kConfigDb) {
        // Only whitelisted collections in config may be sharded (unless we are in test mode)
        uassert(ErrorCodes::IllegalOperation,
                "only special collections in the config db may be sharded",
                nss == NamespaceString::kLogicalSessionsNamespace);
    }

    ShardKeyPattern shardKeyPattern(request.getShardKey().value().getOwned());

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !shardKeyPattern.isHashedPattern() ||
                !(request.getUnique() && request.getUnique().value()));

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !nss.isSystem() || nss == NamespaceString::kLogicalSessionsNamespace ||
                nss.isTemporaryReshardingCollection());

    auto optNumInitialChunks = request.getNumInitialChunks();
    if (optNumInitialChunks) {
        // Ensure numInitialChunks is within valid bounds.
        // Cannot have more than 8192 initial chunks per shard. Setting a maximum of 1,000,000
        // chunks in total to limit the amount of memory this command consumes so there is less
        // danger of an OOM error.

        const auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        const int maxNumInitialChunksForShards = shardIds.size() * 8192;
        const int maxNumInitialChunksTotal = 1000 * 1000;  // Arbitrary limit to memory consumption
        int numChunks = optNumInitialChunks.value();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "numInitialChunks cannot be more than either: "
                              << maxNumInitialChunksForShards << ", 8192 * number of shards; or "
                              << maxNumInitialChunksTotal,
                numChunks >= 0 && numChunks <= maxNumInitialChunksForShards &&
                    numChunks <= maxNumInitialChunksTotal);
    }

    ShardsvrShardCollectionRequest shardsvrShardCollectionRequest;
    shardsvrShardCollectionRequest.set_shardsvrShardCollection(nss);
    shardsvrShardCollectionRequest.setKey(request.getShardKey().value());
    if (request.getUnique())
        shardsvrShardCollectionRequest.setUnique(request.getUnique().value());
    if (request.getNumInitialChunks())
        shardsvrShardCollectionRequest.setNumInitialChunks(request.getNumInitialChunks().value());
    if (request.getPresplitHashedZones())
        shardsvrShardCollectionRequest.setPresplitHashedZones(
            request.getPresplitHashedZones().value());
    if (request.getInitialSplitPoints())
        shardsvrShardCollectionRequest.setInitialSplitPoints(
            request.getInitialSplitPoints().value());

    if (request.getCollation()) {
        inferCollationFromLocalCollection(opCtx, nss, request, &shardsvrShardCollectionRequest);
    }

    return shardCollectionLegacy(opCtx, nss, shardsvrShardCollectionRequest.toBSON(), false);
}

CreateCollectionResponse createCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const ShardsvrCreateCollection& request) {
    uassert(
        ErrorCodes::NotImplemented, "create collection not implemented yet", request.getShardKey());

    DistLockManager::ScopedDistLock dbDistLock(uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx, nss.db(), "shardCollection", DistLockManager::kDefaultLockTimeout)));
    DistLockManager::ScopedDistLock collDistLock(uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx, nss.ns(), "shardCollection", DistLockManager::kDefaultLockTimeout)));

    auto createCollectionCoordinator =
        std::make_shared<CreateCollectionCoordinator>(opCtx, request);
    createCollectionCoordinator->run(opCtx).get(opCtx);
    return createCollectionCoordinator->getResultOnSuccess();
}

class ShardsvrCreateCollectionCommand final : public TypedCommand<ShardsvrCreateCollectionCommand> {
public:
    using Request = ShardsvrCreateCollection;
    using Response = CreateCollectionResponse;

    std::string help() const override {
        return "Internal command. Do not call directly. Creates a collection.";
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
            auto const shardingState = ShardingState::get(opCtx);
            uassertStatusOK(shardingState->canAcceptShardedCommands());

            uassert(
                ErrorCodes::InvalidOptions,
                str::stream()
                    << "_shardsvrCreateCollection must be called with majority writeConcern, got "
                    << request().toBSON(BSONObj()),
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

            uassert(ErrorCodes::NotImplemented,
                    "Create Collection path has not been implemented",
                    request().getShardKey());

            bool useNewPath = [&] {
                // TODO (SERVER-53092): Use the FCV lock in order to "reserve" operation as running
                // in new or legacy mode
                return feature_flags::gShardingFullDDLSupport.isEnabled(
                           serverGlobalParams.featureCompatibility) &&
                    feature_flags::gDisableIncompleteShardingDDLSupport.isEnabled(
                        serverGlobalParams.featureCompatibility);
            }();

            if (!useNewPath) {
                LOGV2_DEBUG(5277911,
                            1,
                            "Running legacy create collection procedure",
                            "namespace"_attr = ns());
                return createCollectionLegacy(opCtx, ns(), request());
            }

            LOGV2_DEBUG(
                5277910, 1, "Running new create collection procedure", "namespace"_attr = ns());
            return createCollection(opCtx, ns(), request());
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} shardsvrCreateCollectionCommand;

}  // namespace
}  // namespace mongo