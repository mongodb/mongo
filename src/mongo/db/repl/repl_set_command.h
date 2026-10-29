// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

class Status;
class Client;
class BSONObj;

namespace repl {

/**
 * Base class for repl set commands.
 */
class [[MONGO_MOD_OPEN]] ReplSetCommand : public BasicCommand {
protected:
    ReplSetCommand(const char* s) : BasicCommand(s) {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override;

    virtual ActionSet getAuthActionSet() const {
        return ActionSet{ActionType::internal};
    }

public:
    bool enableDiagnosticPrintingOnFailure() const override {
        return true;
    }
};

}  // namespace repl
}  // namespace mongo
