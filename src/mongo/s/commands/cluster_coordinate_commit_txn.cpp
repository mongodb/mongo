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

#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/util/assert_util.h"

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
            return NamespaceString(request().getDbName(), "");
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uasserted(ErrorCodes::Unauthorized, "Unauthorized");
        }
    };
} coordinateCommitTransactionCmd;

}  // namespace mongo
