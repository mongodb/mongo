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
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/s/commands/strategy.h"

namespace mongo {
namespace {

/**
 * Base class for mongos index filter commands. Cluster index filter commands don't do much more
 * than forwarding the commands to all shards and combining the results.
 */
class ClusterIndexFilterCmd : public BasicCommand {
    MONGO_DISALLOW_COPYING(ClusterIndexFilterCmd);

public:
    /**
     * Instantiates a command that can be invoked by "name", which will be described by "helpText".
     */
    ClusterIndexFilterCmd(StringData name, std::string helpText)
        : BasicCommand(name), _helpText(std::move(helpText)) {}

    std::string help() const override {
        return _helpText;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        ResourcePattern pattern = parseResourcePattern(dbname, cmdObj);

        if (authzSession->isAuthorizedForActionsOnResource(pattern,
                                                           ActionType::planCacheIndexFilter)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "unauthorized");
    }

    // Cluster plan cache command entry point.
    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        // Dispatch command to all the shards.
        // Targeted shard commands are generally data-dependent but index filter
        // commands are tied to query shape (data has no effect on query shape).
        std::vector<Strategy::CommandResult> results;
        const BSONObj query;
        Strategy::commandOp(opCtx,
                            dbname,
                            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
                            nss.ns(),
                            query,
                            CollationSpec::kSimpleSpec,
                            &results);

        // Set value of first shard result's "ok" field.
        bool clusterCmdResult = true;

        for (auto i = results.begin(); i != results.end(); ++i) {
            const Strategy::CommandResult& cmdResult = *i;

            // XXX: In absence of sensible aggregation strategy,
            //      promote first shard's result to top level.
            if (i == results.begin()) {
                CommandHelpers::filterCommandReplyForPassthrough(cmdResult.result, &result);
                clusterCmdResult = cmdResult.result["ok"].trueValue();
            }

            // Append shard result as a sub object and name the field after the shard id
            result.append(cmdResult.shardTargetId.toString(), cmdResult.result);
        }

        return clusterCmdResult;
    }

private:
    const std::string _helpText;
};

ClusterIndexFilterCmd clusterPlanCacheListFiltersCmd(
    "planCacheListFilters", "Displays index filters for all query shapes in a collection.");

ClusterIndexFilterCmd clusterPlanCacheClearFiltersCmd(
    "planCacheClearFilters",
    "Clears index filter for a single query shape or, "
    "if the query shape is omitted, all filters for the collection.");

ClusterIndexFilterCmd clusterPlanCacheSetFilterCmd(
    "planCacheSetFilter", "Sets index filter for a query shape. Overrides existing index filter.");

}  // namespace
}  // namespace mongo
