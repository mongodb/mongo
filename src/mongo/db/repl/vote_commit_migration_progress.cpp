/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_recipient_coordinator.h"
#include "mongo/db/repl/vote_commit_migration_progress_gen.h"
#include "mongo/logv2/log.h"

namespace mongo::repl {
namespace {

class VoteCommitMigrationProgressCommand final
    : public TypedCommand<VoteCommitMigrationProgressCommand> {
public:
    using Request = VoteCommitMigrationProgress;

    std::string help() const override {
        return "An internal mongod command to coordinate recipient steps of a multitenant "
               "migration";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            BSONObjBuilder result;
            uassertStatusOK(
                repl::ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result));

            const auto& cmd = request();
            LOGV2(6112805,
                  "Received VoteCommitMigrationProgress request",
                  "migrationId"_attr = cmd.getMigrationId(),
                  "from"_attr = cmd.getFrom(),
                  "step"_attr = MigrationProgressStep_serializer(cmd.getStep()),
                  "success"_attr = cmd.getSuccess(),
                  "reason"_attr = cmd.getReason());
            TenantMigrationRecipientCoordinator::get(opCtx)->voteForStep(cmd.getMigrationId(),
                                                                         cmd.getStep(),
                                                                         cmd.getFrom(),
                                                                         cmd.getSuccess(),
                                                                         cmd.getReason());
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };
} VoteCommitMigrationProgressCommand;
}  // namespace
}  // namespace mongo::repl
