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


#include <memory>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/database_version.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand
using namespace fmt::literals;

namespace mongo {
namespace {

std::string rangeString(const BSONObj& min, const BSONObj& max) {
    return "{min: " + min.toString() + " , max" + max.toString() + " }";
}

std::string shardSetString(const std::set<ShardId>& shardIds) {
    std::string result = "[";
    for (auto& shardid : shardIds) {
        result += shardid + ", ";
    }
    return result += "]";
}

class SplitVectorCmd : public BasicCommand {
public:
    SplitVectorCmd() : BasicCommand("splitVector") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return NamespaceStringUtil::deserialize(dbName.tenantId(),
                                                CommandHelpers::parseNsFullyQualified(cmdObj),
                                                SerializationContext::stateDefault());
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(
                     ResourcePattern::forExactNamespace(parseNs(dbName, cmdObj)),
                     ActionType::splitVector)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));
        uassert(ErrorCodes::IllegalOperation,
                "Performing splitVector across dbs isn't supported via mongos",
                nss.dbName() == dbName);

        auto cri =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        BSONObj min = cmdObj.getObjectField("min");
        BSONObj max = cmdObj.getObjectField("max");
        BSONObj keyPattern = cmdObj.getObjectField("keyPattern");

        BSONObj filteredCmdObj = CommandHelpers::filterCommandRequestForPassthrough(cmdObj);
        // splitVector is allowed to run on a sharded cluster only if the range requested
        // belongs to one shard. We target the shard owning the input min chunk and we let the
        // targetted shard figure whether the range is fully owned by itself. In case the
        // constraint is not respected we will get a InvalidOptions as part of the response.
        auto query = [&]() {
            if (!min.isEmpty()) {
                return min;
            }
            // if no min is passed as input, we assume it's the global min of the keyPattern
            // passed as input
            return KeyPattern::fromBSON(keyPattern).globalMin();
        }();
        auto response =
            scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                       nss.dbName(),
                                                       nss,
                                                       cri,
                                                       filteredCmdObj,
                                                       ReadPreferenceSetting::get(opCtx),
                                                       Shard::RetryPolicy::kIdempotent,
                                                       query,
                                                       {} /*collation*/,
                                                       boost::none /*letParameters*/,
                                                       boost::none /*runtimeConstants*/)
                .front();

        auto status = AsyncRequestsSender::Response::getEffectiveStatus(response);
        uassertStatusOK(status);
        result.appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(
            std::move(response.swResponse.getValue().data)));
        return true;
    }
};
MONGO_REGISTER_COMMAND(SplitVectorCmd).forRouter();

}  // namespace
}  // namespace mongo
