
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class FlushRouterConfigCmd : public BasicCommand {
public:
    FlushRouterConfigCmd() : BasicCommand("flushRouterConfig", "flushrouterconfig") {}

    virtual bool slaveOk() const {
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void help(std::stringstream& help) const override {
        help << "Flushes the cached routing information for a single collection, entire database "
                "(and its collections) or all databases, which would cause full reload from the "
                "config server on the next access.\n"
                "Usage:\n"
                "{flushRouterConfig: 1} flushes all databases\n"
                "{flushRouterConfig: 'db'} flushes only the given database (and its collections)\n"
                "{flushRouterconfig: 'db.coll'} flushes only the given collection";
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        ActionSet actions;
        actions.addAction(ActionType::flushRouterConfig);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto const grid = Grid::get(opCtx);
        uassert(ErrorCodes::ShardingStateNotInitialized,
                "Sharding is not enabled",
                grid->isShardingInitialized());

        auto const catalogCache = grid->catalogCache();

        const auto argumentElem = cmdObj.firstElement();
        if (argumentElem.isNumber() || argumentElem.isBoolean()) {
            LOG(0) << "Routing metadata flushed for all databases";
            catalogCache->purgeAllDatabases();
        } else {
            const auto ns = argumentElem.checkAndGetStringData();
            if (nsIsDbOnly(ns)) {
                LOG(0) << "Routing metadata flushed for database " << ns;
                catalogCache->purgeDatabase(ns);
            } else {
                const NamespaceString nss(ns);
                LOG(0) << "Routing metadata flushed for collection " << nss;
                catalogCache->purgeCollection(nss);
            }
        }

        result.appendBool("flushed", true);
        return true;
    }

} flushRouterConfigCmd;

}  // namespace
}  // namespace mongo
