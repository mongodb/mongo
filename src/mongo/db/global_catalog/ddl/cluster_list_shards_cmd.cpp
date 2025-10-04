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
#include "mongo/db/global_catalog/ddl/list_shards_gen.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/util/assert_util.h"

#include <string>
#include <vector>

namespace mongo {
namespace {

class ListShardsCmd : public BasicCommand {
public:
    // using Request = ListShardsRequest;

    ListShardsCmd() : BasicCommand("listShards", "listshards") {}

    std::string help() const override {
        return "list all shards of the system";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::listShards)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        const auto request =
            ListShardsRequest::parse(cmdObj, IDLParserContext("listShardsRequest"));

        const BSONObj filter = [&] {
            const auto request_filter = request.getFilter();
            if (!request_filter) {
                return BSONObj();
            }
            // If filter is draining: false, we transform it to $ne: true to ensure all non-draining
            // shards are returned as the draining field is either unset or false when a shard isn't
            // draining.
            if (request_filter->getDraining().is_initialized() &&
                !(*request_filter->getDraining())) {
                return BSON(ShardType::draining.ne(true));
            }
            return request_filter->toBSON();
        }();
        const auto opTimeWithShards = Grid::get(opCtx)->catalogClient()->getAllShards(
            opCtx, repl::ReadConcernLevel::kMajorityReadConcern, filter);

        BSONArrayBuilder shardsArr(result.subarrayStart("shards"));
        for (const auto& shard : opTimeWithShards.value) {
            shardsArr.append(shard.toBSON());
        }
        shardsArr.doneFast();

        return true;
    }
};
MONGO_REGISTER_COMMAND(ListShardsCmd).forRouter();

}  // namespace
}  // namespace mongo
