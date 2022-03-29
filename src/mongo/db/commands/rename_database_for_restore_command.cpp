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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/catalog_control.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_database_for_restore_command_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

class RenameDatabaseForRestoreCmd
    : public BasicCommandWithRequestParser<RenameDatabaseForRestoreCmd> {
public:
    using Request = RenameDatabaseForRestoreCommand;

    bool skipApiVersionCheck() const override {
        // Internal command used by the restore procedure.
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const std::string& db,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) final {
        uassert(ErrorCodes::CommandFailed,
                "Cannot run the 'renameDatabaseForRestore' command when the "
                "'featureFlagDatabaseRenameDuringRestore' is disabled.",
                feature_flags::gDatabaseRenameDuringRestore.isEnabled(
                    serverGlobalParams.featureCompatibility));

        uassert(ErrorCodes::CommandFailed,
                "This command can only be used in standalone mode",
                !repl::ReplicationCoordinator::get(opCtx)->getSettings().usingReplSets());

        uassert(ErrorCodes::CommandFailed,
                "This command can only be run during a restore procedure",
                storageGlobalParams.restore);

        const auto* cmd = &requestParser.request();
        auto from = cmd->getFrom();
        auto to = cmd->getTo();

        LOGV2(6460300, "CMD: renameDatabaseForRestore", "from"_attr = from, "to"_attr = to);

        return true;
    }

    void validateResult(const BSONObj& resultObj) final {
        return;
    }

} renameDatabaseForRestoreCmd;

}  // namespace
}  // namespace mongo
