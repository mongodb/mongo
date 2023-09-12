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


#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

class FlushRouterConfigCmd : public BasicCommand {
public:
    FlushRouterConfigCmd() : BasicCommand("flushRouterConfig", "flushrouterconfig") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "Flushes the cached routing information for a single collection, entire database "
               "(and its collections) or all databases, which would cause full reload from the "
               "config server on the next access.\n"
               "Usage:\n"
               "{flushRouterConfig: 1} flushes all databases\n"
               "{flushRouterConfig: 'db'} flushes only the given database (and its collections)\n"
               "{flushRouterconfig: 'db.coll'} flushes only the given collection";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(dbName.tenantId()),
                ActionType::flushRouterConfig)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto const grid = Grid::get(opCtx);
        uassert(ErrorCodes::ShardingStateNotInitialized,
                "Sharding is not enabled",
                grid->isShardingInitialized());

        auto const catalogCache = grid->catalogCache();

        const auto argumentElem = cmdObj.firstElement();
        if (argumentElem.isNumber() || argumentElem.isBoolean()) {
            LOGV2(22761, "Routing metadata flushed for all databases");
            catalogCache->purgeAllDatabases();
        } else {
            const auto ns = argumentElem.checkAndGetStringData();
            const auto nss = NamespaceStringUtil::deserialize(
                boost::none, ns, SerializationContext::stateCommandRequest());
            if (nsIsDbOnly(ns)) {
                LOGV2(22762,
                      "Routing metadata flushed for database {db}",
                      "Routing metadata flushed for database",
                      "db"_attr = toStringForLogging(nss));
                catalogCache->purgeDatabase(nss.dbName());
            } else {
                LOGV2(22763,
                      "Routing metadata flushed for collection {namespace}",
                      "Routing metadata flushed for collection",
                      logAttrs(nss));
                catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
                LOGV2(7343300, "Index information flushed for collection", logAttrs(nss));
                catalogCache->invalidateIndexEntry_LINEARIZABLE(nss);
            }
        }

        result.appendBool("flushed", true);
        return true;
    }
};
MONGO_REGISTER_COMMAND(FlushRouterConfigCmd).forRouter().forShard();

}  // namespace
}  // namespace mongo
