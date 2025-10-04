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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/shard_collection_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>

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

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forExactNamespace(parseNs(dbName, cmdObj)),
                     ActionType::enableSharding)) {
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

        uassert(5731501,
                "Sharding a buckets collection is not allowed",
                !nss.isTimeseriesBucketsCollection());

        uassert(6464401,
                "Sharding a Queryable Encryption state collection is not allowed",
                !nss.isFLE2StateCollection());

        auto clusterRequest = ShardCollection::parse(cmdObj, IDLParserContext(""));
        ShardsvrCreateCollectionRequest serverRequest;
        serverRequest.setShardKey(clusterRequest.getKey());
        serverRequest.setUnique(clusterRequest.getUnique());
        serverRequest.setNumInitialChunks(clusterRequest.getNumInitialChunks());
        serverRequest.setPresplitHashedZones(clusterRequest.getPresplitHashedZones());
        serverRequest.setCollation(clusterRequest.getCollation());
        serverRequest.setTimeseries(clusterRequest.getTimeseries());
        serverRequest.setCollectionUUID(clusterRequest.getCollectionUUID());
        serverRequest.setImplicitlyCreateIndex(clusterRequest.getImplicitlyCreateIndex());
        serverRequest.setEnforceUniquenessCheck(clusterRequest.getEnforceUniquenessCheck());

        ShardsvrCreateCollection shardsvrCreateCommand(nss);
        shardsvrCreateCommand.setShardsvrCreateCollectionRequest(std::move(serverRequest));
        shardsvrCreateCommand.setDbName(nss.dbName());

        cluster::createCollection(opCtx, std::move(shardsvrCreateCommand));

        // Add only collectionsharded as a response parameter and remove the version to maintain the
        // same format as before.
        result.append("collectionsharded",
                      NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        return true;
    }
};
MONGO_REGISTER_COMMAND(ShardCollectionCmd).forRouter();

}  // namespace
}  // namespace mongo
