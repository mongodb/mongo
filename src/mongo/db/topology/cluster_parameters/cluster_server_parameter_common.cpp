// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/database_name.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/shard_role/ddl/list_databases_for_all_tenants_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
using namespace std::literals::string_view_literals;

StatusWith<std::set<boost::optional<TenantId>>> getTenantsWithConfigDbsOnShard(
    OperationContext* opCtx, Shard& shard) {
    if (!gMultitenancySupport) {
        return std::set<boost::optional<TenantId>>{boost::none};
    }
    // Find all tenant config databases.
    ListDatabasesForAllTenantsCommand listDbCommand;
    listDbCommand.setDbName(DatabaseName::kAdmin);
    listDbCommand.setFilter(BSON("name"sv << "config"));
    listDbCommand.setNameOnly(true);
    std::set<boost::optional<TenantId>> tenantIds;

    auto swListDbResponse =
        shard.runCommandWithIndefiniteRetries(opCtx,
                                              ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                              DatabaseName::kAdmin,
                                              listDbCommand.toBSON(),
                                              Shard::RetryPolicy::kIdempotent);
    if (!swListDbResponse.isOK()) {
        return swListDbResponse.getStatus();
    }
    std::vector<BSONElement> databases =
        swListDbResponse.getValue().response["databases"sv].Array();
    LOGV2_DEBUG(6831301,
                2,
                "ListDatabasesForAllTenants w/ default executor finished",
                "response"_attr = databases);

    std::transform(databases.begin(),
                   databases.end(),
                   std::inserter(tenantIds, tenantIds.end()),
                   [](const BSONElement& elem) -> boost::optional<TenantId> {
                       auto tenantElem = elem.Obj()["tenantId"sv];
                       if (tenantElem.eoo()) {
                           return boost::none;
                       } else {
                           // Tenant field is non-empty, put real tenant ID
                           return TenantId(tenantElem.OID());
                       }
                   });

    return tenantIds;
}

StatusWith<std::set<boost::optional<TenantId>>> getTenantsWithConfigDbsOnShard(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor) {
    if (!gMultitenancySupport) {
        return std::set<boost::optional<TenantId>>{boost::none};
    }
    // Find all tenant config databases.
    ListDatabasesForAllTenantsCommand listDbCommand;
    listDbCommand.setDbName(DatabaseName::kAdmin);
    listDbCommand.setFilter(BSON("name"sv << "config"));
    listDbCommand.setNameOnly(true);
    std::set<boost::optional<TenantId>> tenantIds;

    auto swHost = targeter.findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, {});
    if (!swHost.isOK()) {
        return swHost.getStatus();
    }
    auto host = std::move(swHost.getValue());
    executor::RemoteCommandRequest request(
        host, DatabaseName::kAdmin, listDbCommand.toBSON(), opCtx);

    executor::RemoteCommandResponse response(
        host, Status(ErrorCodes::InternalError, "Internal error running command"));

    auto swCallbackHandle = executor->scheduleRemoteCommand(
        request, [&response](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            response = args.response;
        });
    if (!swCallbackHandle.isOK()) {
        return swCallbackHandle.getStatus();
    }

    // Block until the command is carried out
    executor->wait(swCallbackHandle.getValue());
    if (!response.isOK()) {
        return response.status;
    }
    std::vector<BSONElement> databases;
    response.data["databases"sv].Obj().elems(databases);
    LOGV2_DEBUG(6831302,
                2,
                "ListDatabasesForAllTenants w/ special executor finished",
                "response"_attr = databases);

    std::transform(databases.begin(),
                   databases.end(),
                   std::inserter(tenantIds, tenantIds.end()),
                   [](const BSONElement& elem) -> boost::optional<TenantId> {
                       auto tenantElem = elem.Obj()["tenantId"sv];
                       if (tenantElem.eoo()) {
                           return boost::none;
                       } else {
                           // Tenant field is non-empty, put real tenant ID
                           return TenantId(tenantElem.OID());
                       }
                   });

    return tenantIds;
}

}  // namespace mongo
