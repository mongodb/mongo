// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
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
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <iostream>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Resets the commitQuorum set on an index build identified by the list of index names that were
 * previously specified in a createIndexes request.
 *
 * {
 *     setIndexCommitQuorum: coll,
 *     indexNames: ["x_1", "y_1", "xIndex", "someindexname"],
 *     commitQuorum: "majority" / 3 / {"replTagName": "replTagValue"},
 * }
 */
class SetIndexCommitQuorumCommand : public BasicCommand {
public:
    SetIndexCommitQuorumCommand() : BasicCommand("setIndexCommitQuorum") {}

    std::string help() const override {
        std::stringstream ss;
        ss << "Resets the commitQuorum for the given index builds in a collection. Usage:"
           << std::endl
           << "{" << std::endl
           << "    setIndexCommitQuorum: <string> collection name," << std::endl
           << "    indexNames: array<string> list of index names," << std::endl
           << "    commitQuorum: <string|number|object> option to define the required quorum for"
           << std::endl
           << "                  the index builds to commit" << std::endl
           << "}";
        return ss.str();
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return true;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(nss),
                                                    ActionType::createIndex)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        LOGV2_DEBUG(
            22757, 1, "setIndexCommitQuorum", logAttrs(nss), "command"_attr = redact(cmdObj));

        sharding::router::CollectionRouter router(opCtx, nss);
        return router.routeWithRoutingContext(
            getName(), [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
                    opCtx,
                    routingCtx,
                    nss,
                    applyReadWriteConcern(
                        opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
                    ReadPreferenceSetting::get(opCtx),
                    Shard::RetryPolicy::kNotIdempotent,
                    BSONObj() /*query*/,
                    BSONObj() /*collation*/,
                    boost::none /*letParameters*/,
                    boost::none /*runtimeConstants*/);

                std::string errmsg;
                const bool ok =
                    appendRawResponses(opCtx, &errmsg, &result, std::move(shardResponses))
                        .responseOK;
                CommandHelpers::appendSimpleCommandStatus(result, ok, errmsg);

                if (ok) {
                    LOGV2(5688700, "Index commit quorums set", logAttrs(nss));
                }

                return ok;
            });
    }
};
MONGO_REGISTER_COMMAND(SetIndexCommitQuorumCommand).forRouter();

}  // namespace
}  // namespace mongo
