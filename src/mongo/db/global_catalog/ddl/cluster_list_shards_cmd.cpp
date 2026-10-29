// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
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
            opCtx, repl::ReadConcernArgs::kMajority, filter);

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
