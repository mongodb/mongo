// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_set_command.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"

namespace mongo {
namespace repl {

Status ReplSetCommand::checkAuthForOperation(OperationContext* opCtx,
                                             const DatabaseName& dbName,
                                             const BSONObj&) const {
    if (!AuthorizationSession::get(opCtx->getClient())
             ->isAuthorizedForActionsOnResource(
                 ResourcePattern::forClusterResource(dbName.tenantId()), getAuthActionSet())) {
        return {ErrorCodes::Unauthorized, "Unauthorized"};
    }

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
