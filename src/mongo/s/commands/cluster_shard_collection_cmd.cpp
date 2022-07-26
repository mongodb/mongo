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


#include "mongo/platform/basic.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/commands/shard_collection_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ShardCollectionCmd : public BasicCommand {
public:
    ShardCollectionCmd() : BasicCommand("shardCollection", "shardcollection") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Shard a collection. Requires key. Optional unique.";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::enableSharding)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        uassert(5731501,
                "Sharding a buckets collection is not allowed",
                !nss.isTimeseriesBucketsCollection());

        uassert(6464401,
                "Sharding a Queryable Encryption state collection is not allowed",
                !nss.isFLE2StateCollection());

        auto shardCollRequest = ShardCollection::parse(IDLParserContext("ShardCollection"), cmdObj);

        ShardsvrCreateCollection shardsvrCollRequest(nss);
        CreateCollectionRequest requestParamsObj;
        requestParamsObj.setShardKey(shardCollRequest.getKey());
        requestParamsObj.setUnique(shardCollRequest.getUnique());
        requestParamsObj.setNumInitialChunks(shardCollRequest.getNumInitialChunks());
        requestParamsObj.setPresplitHashedZones(shardCollRequest.getPresplitHashedZones());
        requestParamsObj.setCollation(shardCollRequest.getCollation());
        requestParamsObj.setTimeseries(shardCollRequest.getTimeseries());
        requestParamsObj.setCollectionUUID(shardCollRequest.getCollectionUUID());
        requestParamsObj.setImplicitlyCreateIndex(shardCollRequest.getImplicitlyCreateIndex());
        requestParamsObj.setEnforceUniquenessCheck(shardCollRequest.getEnforceUniquenessCheck());
        shardsvrCollRequest.setCreateCollectionRequest(std::move(requestParamsObj));
        shardsvrCollRequest.setDbName(nss.db());

        cluster::createCollection(opCtx, shardsvrCollRequest);

        // Add only collectionsharded as a response parameter and remove the version to maintain the
        // same format as before.
        result.append("collectionsharded", nss.toString());
        return true;
    }

} shardCollectionCmd;

}  // namespace
}  // namespace mongo
