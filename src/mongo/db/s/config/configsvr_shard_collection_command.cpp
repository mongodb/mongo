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

#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/shard_key_util.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_client.h"
#include "mongo/s/grid.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

using std::string;

const long long kMaxSizeMBDefault = 0;

/**
 * Validates the options specified in the request.
 *
 * WARNING: After validating the request's collation, replaces it with the collection default
 * collation.
 */
void validateAndDeduceFullRequestOptions(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const ShardKeyPattern& shardKeyPattern,
                                         int numShards,
                                         const std::shared_ptr<Shard>& primaryShard,
                                         ConfigsvrShardCollectionRequest* request) {
    uassert(
        ErrorCodes::InvalidOptions, "cannot have empty shard key", !request->getKey().isEmpty());

    // Ensure that hashed and unique are not both set.
    uassert(ErrorCodes::InvalidOptions,
            "Hashed shard keys cannot be declared unique. It's possible to ensure uniqueness on "
            "the hashed field by declaring an additional (non-hashed) unique index on the field.",
            !shardKeyPattern.isHashedPattern() || !request->getUnique());

    // Ensure the namespace is valid.
    uassert(ErrorCodes::IllegalOperation,
            "can't shard system namespaces",
            !nss.isSystem() || nss == NamespaceString::kLogicalSessionsNamespace);

    // Ensure the collation is valid. Currently we only allow the simple collation.
    bool simpleCollationSpecified = false;
    if (request->getCollation()) {
        auto& collation = *request->getCollation();
        auto collator = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collation));
        uassert(ErrorCodes::BadValue,
                str::stream() << "The collation for shardCollection must be {locale: 'simple'}, "
                              << "but found: " << collation,
                !collator);
        simpleCollationSpecified = true;
    }

    // Ensure numInitialChunks is within valid bounds.
    // Cannot have more than 8192 initial chunks per shard. Setting a maximum of 1,000,000
    // chunks in total to limit the amount of memory this command consumes so there is less
    // danger of an OOM error.
    const int maxNumInitialChunksForShards = numShards * 8192;
    const int maxNumInitialChunksTotal = 1000 * 1000;  // Arbitrary limit to memory consumption
    int numChunks = request->getNumInitialChunks();
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "numInitialChunks cannot be more than either: "
                          << maxNumInitialChunksForShards << ", 8192 * number of shards; or "
                          << maxNumInitialChunksTotal,
            numChunks >= 0 && numChunks <= maxNumInitialChunksForShards &&
                numChunks <= maxNumInitialChunksTotal);

    // Retrieve the collection metadata in order to verify that it is legal to shard this
    // collection.
    BSONObj res;
    {
        auto listCollectionsCmd =
            BSON("listCollections" << 1 << "filter" << BSON("name" << nss.coll()));
        auto allRes = uassertStatusOK(primaryShard->runExhaustiveCursorCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            nss.db().toString(),
            listCollectionsCmd,
            Milliseconds(-1)));
        const auto& all = allRes.docs;
        if (!all.empty()) {
            res = all.front().getOwned();
        }
    }

    BSONObj defaultCollation;

    if (!res.isEmpty()) {
        // Check that namespace is not a view.
        {
            std::string namespaceType;
            uassertStatusOK(bsonExtractStringField(res, "type", &namespaceType));
            uassert(ErrorCodes::CommandNotSupportedOnView,
                    "Views cannot be sharded.",
                    namespaceType != "view");
        }

        BSONObj collectionOptions;
        if (res["options"].type() == BSONType::Object) {
            collectionOptions = res["options"].Obj();
        }

        // Check that collection is not capped.
        uassert(ErrorCodes::InvalidOptions,
                "can't shard a capped collection",
                !collectionOptions["capped"].trueValue());

        // Get collection default collation.
        BSONElement collationElement;
        auto status = bsonExtractTypedField(
            collectionOptions, "collation", BSONType::Object, &collationElement);
        if (status.isOK()) {
            defaultCollation = collationElement.Obj().getOwned();
            uassert(ErrorCodes::BadValue,
                    "Default collation in collection metadata cannot be empty.",
                    !defaultCollation.isEmpty());
        } else if (status != ErrorCodes::NoSuchKey) {
            uassertStatusOK(status);
        }

        // If the collection has a non-simple default collation but the user did not specify the
        // simple collation explicitly, return an error.
        uassert(ErrorCodes::BadValue,
                str::stream() << "Collection has default collation: "
                              << collectionOptions["collation"]
                              << ". Must specify collation {locale: 'simple'}",
                defaultCollation.isEmpty() || simpleCollationSpecified);
    }

    // Once the request's collation has been validated as simple or unset, replace it with the
    // deduced collection default collation.
    request->setCollation(defaultCollation.getOwned());
}

/**
 * Internal sharding command run on config servers to shard a collection.
 */
class ConfigSvrShardCollectionCommand : public BasicCommand {
public:
    ConfigSvrShardCollectionCommand() : BasicCommand("_configsvrShardCollection") {}

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Shards a collection. Requires key. Optional unique. Sharding must "
               "already be enabled for the database";
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrShardCollection can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "_configsvrShardCollection must be called with majority writeConcern, got "
                    << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        const NamespaceString nss(parseNs(dbname, cmdObj));
        auto request = ConfigsvrShardCollectionRequest::parse(
            IDLParserErrorContext("ConfigsvrShardCollectionRequest"), cmdObj);

        auto const catalogManager = ShardingCatalogManager::get(opCtx);
        auto const catalogCache = Grid::get(opCtx)->catalogCache();
        auto const catalogClient = Grid::get(opCtx)->catalogClient();
        auto shardRegistry = Grid::get(opCtx)->shardRegistry();

        // Make the distlocks boost::optional so that they can be released by being reset below.
        boost::optional<DistLockManager::ScopedDistLock> dbDistLock(
            uassertStatusOK(catalogClient->getDistLockManager()->lock(
                opCtx, nss.db(), "shardCollection", DistLockManager::kDefaultLockTimeout)));
        boost::optional<DistLockManager::ScopedDistLock> collDistLock(
            uassertStatusOK(catalogClient->getDistLockManager()->lock(
                opCtx, nss.ns(), "shardCollection", DistLockManager::kDefaultLockTimeout)));

        // Ensure sharding is allowed on the database.
        // Until all metadata commands are on the config server, the CatalogCache on the config
        // server may be stale. Read the database entry directly rather than purging and reloading
        // the database into the CatalogCache, which is very expensive.
        auto dbType =
            uassertStatusOK(
                Grid::get(opCtx)->catalogClient()->getDatabase(
                    opCtx, nss.db().toString(), repl::ReadConcernArgs::get(opCtx).getLevel()))
                .value;
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "sharding not enabled for db " << nss.db(),
                dbType.getSharded());

        // Get variables required throughout this command.

        auto proposedKey(request.getKey().getOwned());
        ShardKeyPattern shardKeyPattern(proposedKey);

        std::vector<ShardId> shardIds;
        shardRegistry->getAllShardIds(opCtx, &shardIds);
        uassert(ErrorCodes::IllegalOperation,
                "cannot shard collections before there are shards",
                !shardIds.empty());

        // Handle collections in the config db separately.
        if (nss.db() == NamespaceString::kConfigDb) {
            // Only whitelisted collections in config may be sharded (unless we are in test mode)
            uassert(ErrorCodes::IllegalOperation,
                    "only special collections in the config db may be sharded",
                    nss == NamespaceString::kLogicalSessionsNamespace);

            auto configShard = uassertStatusOK(shardRegistry->getShard(opCtx, dbType.getPrimary()));
            auto countCmd = BSON("count" << nss.coll());
            auto countRes = uassertStatusOK(
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                        nss.db().toString(),
                                        countCmd,
                                        Shard::RetryPolicy::kIdempotent));
            auto numDocs = countRes.response["n"].Int();

            // If this is a collection on the config db, it must be empty to be sharded,
            // otherwise we might end up with chunks on the config servers.
            uassert(ErrorCodes::IllegalOperation,
                    "collections in the config db must be empty to be sharded",
                    numDocs == 0);
        }

        // For the config db, pick a new host shard for this collection, otherwise
        // make a connection to the real primary shard for this database.
        auto primaryShardId = [&]() {
            if (nss.db() == NamespaceString::kConfigDb) {
                return shardIds[0];
            } else {
                return dbType.getPrimary();
            }
        }();

        auto primaryShard = uassertStatusOK(shardRegistry->getShard(opCtx, primaryShardId));

        // Step 1.
        validateAndDeduceFullRequestOptions(
            opCtx, nss, shardKeyPattern, shardIds.size(), primaryShard, &request);

        // The collation option should have been set to the collection default collation after being
        // validated.
        invariant(request.getCollation());

        boost::optional<UUID> uuid;

        // The primary shard will read the config.tags collection so we need to lock the zone
        // mutex.
        Lock::ExclusiveLock lk = catalogManager->lockZoneMutex(opCtx);

        ShardsvrShardCollection shardsvrShardCollectionRequest;
        shardsvrShardCollectionRequest.set_shardsvrShardCollection(nss);
        shardsvrShardCollectionRequest.setKey(request.getKey());
        shardsvrShardCollectionRequest.setUnique(request.getUnique());
        shardsvrShardCollectionRequest.setNumInitialChunks(request.getNumInitialChunks());
        shardsvrShardCollectionRequest.setPresplitHashedZones(request.getPresplitHashedZones());
        shardsvrShardCollectionRequest.setInitialSplitPoints(request.getInitialSplitPoints());
        shardsvrShardCollectionRequest.setCollation(request.getCollation());
        shardsvrShardCollectionRequest.setGetUUIDfromPrimaryShard(
            request.getGetUUIDfromPrimaryShard());

        auto cmdResponse = uassertStatusOK(primaryShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            CommandHelpers::appendMajorityWriteConcern(CommandHelpers::appendPassthroughFields(
                cmdObj, shardsvrShardCollectionRequest.toBSON())),
            Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(cmdResponse.commandStatus);

        auto shardCollResponse = ShardsvrShardCollectionResponse::parse(
            IDLParserErrorContext("ShardsvrShardCollectionResponse"), cmdResponse.response);
        uuid = std::move(shardCollResponse.getCollectionUUID());

        result << "collectionsharded" << nss.ns();
        if (uuid) {
            result << "collectionUUID" << *uuid;
        }

        auto routingInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfoWithRefresh(opCtx, nss));
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Collection was successfully written as sharded but got dropped before it "
                "could be evenly distributed",
                routingInfo.cm());

        return true;
    }

} configsvrShardCollectionCmd;

}  // namespace
}  // namespace mongo
