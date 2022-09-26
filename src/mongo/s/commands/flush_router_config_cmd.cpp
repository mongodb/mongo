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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"

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
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
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
            if (nsIsDbOnly(ns)) {
                LOGV2(22762,
                      "Routing metadata flushed for database {db}",
                      "Routing metadata flushed for database",
                      "db"_attr = ns);
                catalogCache->purgeDatabase(ns);
            } else {
                const NamespaceString nss(ns);
                LOGV2(22763,
                      "Routing metadata flushed for collection {namespace}",
                      "Routing metadata flushed for collection",
                      "namespace"_attr = nss);
                catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
            }
        }

        result.appendBool("flushed", true);
        return true;
    }

} flushRouterConfigCmd;

}  // namespace
}  // namespace mongo
