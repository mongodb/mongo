/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/cluster_parameters/cluster_server_parameter_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/ddl/list_databases_for_all_tenants_gen.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

StatusWith<std::set<boost::optional<TenantId>>> getTenantsWithConfigDbsOnShard(
    OperationContext* opCtx, Shard& shard) {
    if (!gMultitenancySupport) {
        return std::set<boost::optional<TenantId>>{boost::none};
    }
    // Find all tenant config databases.
    ListDatabasesForAllTenantsCommand listDbCommand;
    listDbCommand.setDbName(DatabaseName::kAdmin);
    listDbCommand.setFilter(BSON("name"_sd << "config"));
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
        swListDbResponse.getValue().response["databases"_sd].Array();
    LOGV2_DEBUG(6831301,
                2,
                "ListDatabasesForAllTenants w/ default executor finished",
                "response"_attr = databases);

    std::transform(databases.begin(),
                   databases.end(),
                   std::inserter(tenantIds, tenantIds.end()),
                   [](const BSONElement& elem) -> boost::optional<TenantId> {
                       auto tenantElem = elem.Obj()["tenantId"_sd];
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
    listDbCommand.setFilter(BSON("name"_sd << "config"));
    listDbCommand.setNameOnly(true);
    std::set<boost::optional<TenantId>> tenantIds;

    auto swHost = targeter.findHost(opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
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
    response.data["databases"_sd].Obj().elems(databases);
    LOGV2_DEBUG(6831302,
                2,
                "ListDatabasesForAllTenants w/ special executor finished",
                "response"_attr = databases);

    std::transform(databases.begin(),
                   databases.end(),
                   std::inserter(tenantIds, tenantIds.end()),
                   [](const BSONElement& elem) -> boost::optional<TenantId> {
                       auto tenantElem = elem.Obj()["tenantId"_sd];
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
