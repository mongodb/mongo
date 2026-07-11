// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/set_index_commit_quorum_gen.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <iostream>
#include <memory>
#include <string>

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
           << "}" << std::endl
           << "This command is useful if the commitQuorum of an active index build is no longer "
              "possible or desirable (replica set membership has changed), or potential secondary "
              "replication lag has become a greater concern";
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
            const auto& provider =
                rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
            uassert(ErrorCodes::CommandNotSupported,
                    str::stream() << "setIndexCommitQuorum is not supported in this storage mode: "
                                  << provider.name(),
                    !provider.mustUsePrimaryDrivenIndexBuilds());

            uassertStatusOK(
                IndexBuildsCoordinator::get(opCtx)->setCommitQuorum(opCtx,
                                                                    request().getNamespace(),
                                                                    request().getIndexNames(),
                                                                    request().getCommitQuorum()));
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
};
MONGO_REGISTER_COMMAND(SetIndexCommitQuorumCommand).forShard();

}  // namespace
}  // namespace mongo
