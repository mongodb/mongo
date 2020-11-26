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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/commands.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

class DropCmd : public BasicCommand {
public:
    DropCmd() : BasicCommand("drop") {}

    virtual const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::dropCollection);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto parsed = Drop::parse(IDLParserErrorContext("drop"), cmdObj);
        const auto& nss = parsed.getNamespace();

        uassert(ErrorCodes::IllegalOperation,
                "Cannot drop collection in config database",
                nss.db() != NamespaceString::kConfigDb);

        uassert(ErrorCodes::IllegalOperation,
                "Cannot drop collection in admin database",
                nss.db() != NamespaceString::kAdminDb);

        try {
            // Invalidate the routing table cache entry for this collection so that we reload it the
            // next time it is accessed, even if sending the command to the config server fails due
            // to e.g. a NetworkError.
            ON_BLOCK_EXIT([opCtx, nss] {
                Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
            });

            const auto dbInfo =
                uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, dbname));

            // Send it to the primary shard
            ShardsvrDropCollection dropCollectionCommand(nss);
            dropCollectionCommand.setDbName(dbname);

            auto cmdResponse = executeCommandAgainstDatabasePrimary(
                opCtx,
                dbname,
                dbInfo,
                CommandHelpers::appendMajorityWriteConcern(dropCollectionCommand.toBSON({}),
                                                           opCtx->getWriteConcern()),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kIdempotent);

            const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);

            CommandHelpers::filterCommandReplyForPassthrough(remoteResponse.data, &result);

            return true;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // If the namespace isn't found, treat the drop as a success but inform about the
            // failure.
            result.append("info", "database does not exist");
            return true;
        }
    }

} clusterDropCmd;

}  // namespace
}  // namespace mongo
