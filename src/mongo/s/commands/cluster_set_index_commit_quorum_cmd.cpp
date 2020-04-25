/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <iostream>
#include <string>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/set_index_commit_quorum_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"

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
class SetIndexCommitQuorumCommand final : public TypedCommand<SetIndexCommitQuorumCommand> {
public:
    using Request = SetIndexCommitQuorum;

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

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            BSONObj cmdObj = request().toBSON(BSONObj());
            LOGV2_DEBUG(22757,
                        1,
                        "setIndexCommitQuorum",
                        "namespace"_attr = request().getNamespace(),
                        "command"_attr = redact(cmdObj));

            scatterGatherOnlyVersionIfUnsharded(
                opCtx,
                request().getNamespace(),
                applyReadWriteConcern(
                    opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kNotIdempotent);
        }

    private:
        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forExactNamespace(request().getNamespace()),
                            ActionType::createIndex));
        }
    };

} setCommitQuorumCmd;

}  // namespace
}  // namespace mongo
