// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {

class CoordinateCommitTransactionCmd : public TypedCommand<CoordinateCommitTransactionCmd> {
public:
    using Request = CoordinateCommitTransaction;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    bool isTransactionCommand() const final {
        return true;
    }

    bool shouldCheckoutSession() const final {
        return false;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // Unimplemented in mongos, only serve as a stub for functions like
            // isTransactionCommand. An example where isTransactionCommand can be called on this
            // command is in the TransactionRouter.
            uasserted(6510100, "Not implemented");
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uasserted(ErrorCodes::Unauthorized, "Unauthorized");
        }
    };
};
MONGO_REGISTER_COMMAND(CoordinateCommitTransactionCmd).forRouter();

}  // namespace mongo
