// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/current_op_common.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/s/query/planner/cluster_aggregate.h"

#include <vector>

namespace mongo {
namespace {

class ClusterCurrentOpCommand final : public CurrentOpCommandBase {
    ClusterCurrentOpCommand(const ClusterCurrentOpCommand&) = delete;
    ClusterCurrentOpCommand& operator=(const ClusterCurrentOpCommand&) = delete;

public:
    ClusterCurrentOpCommand() = default;

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const final {
        bool isAuthorized =
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(
                    ResourcePattern::forClusterResource(dbName.tenantId()), ActionType::inprog);

        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

private:
    void modifyPipeline(std::vector<BSONObj>* pipeline) const final {
        BSONObjBuilder sortBuilder;

        BSONObjBuilder sortSpecBuilder(sortBuilder.subobjStart("$sort"));
        sortSpecBuilder.append("shard", 1);
        sortSpecBuilder.doneFast();

        pipeline->push_back(sortBuilder.obj());
    }

    StatusWith<CursorResponse> runAggregation(OperationContext* opCtx,
                                              AggregateCommandRequest& request) const final {
        auto nss = request.getNamespace();

        BSONObjBuilder responseBuilder;

        auto status = ClusterAggregate::runAggregate(
            opCtx,
            ClusterAggregate::Namespaces{nss, nss},
            request,
            {request},
            {Privilege(ResourcePattern::forClusterResource(nss.tenantId()), ActionType::inprog)},
            boost::none, /* verbosity */
            &responseBuilder);

        if (!status.isOK()) {
            return status;
        }

        CommandHelpers::appendSimpleCommandStatus(responseBuilder, true);

        return CursorResponse::parseFromBSON(responseBuilder.obj());
    }
};

MONGO_REGISTER_COMMAND(ClusterCurrentOpCommand).forRouter();
}  // namespace
}  // namespace mongo
