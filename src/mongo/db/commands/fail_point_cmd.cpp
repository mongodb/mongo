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

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/request_types/wait_for_fail_point_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <string>

namespace mongo {

/**
 * Test-only command for modifying installed fail points.
 *
 * Requires the 'enableTestCommands' server parameter to be set. See docs/test_commands.md.
 *
 * Format
 * {
 *    configureFailPoint: <string>, // name of the fail point.
 *
 *    mode: <string|Object>, // the new mode to set. Can have one of the following format:
 *
 *        - 'off' - disable fail point.
 *
 *        - 'alwaysOn' - fail point is always active.
 *
 *        - { activationProbability: <n> } - double n. [0 <= n <= 1]
 *          n: the probability that the fail point will fire.  0=never, 1=always.
 *
 *        - { times: <n> } - int32 n. n > 0. n: # of passes the fail point remains active.
 *
 *        - { skip: <n> } - int32 n. n > 0. n: # of passes before the fail point activates
 *          and remains active.
 *
 *    data: <Object> // optional arbitrary object to inject into the failpoint.
 *        When activated, the FailPoint can read this data and it can be used to inform
 *        the specific action taken by the code under test.
 * }
 */
class FaultInjectCmd : public BasicCommand {
public:
    FaultInjectCmd() : BasicCommand("configureFailPoint") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool requiresAuth() const override {
        return false;
    }

    // No auth needed because it only works when enabled via command line.
    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    std::string help() const override {
        return "modifies the settings of a fail point";
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const std::string failPointName(cmdObj.firstElement().str());
        const auto timesEntered = setGlobalFailPoint(failPointName, cmdObj);
        result.appendNumber("count", timesEntered);
        return true;
    }
};

/**
 * Command for waiting for installed fail points.
 *
 * For number of additional times entered > 1, this command is only guaranteed to work
 * correctly if the code that enters the fail point uses the FailPoint API correctly.
 * That is, the code can only use one of shouldFail, pauseWhileSet, scopedIf, scoped,
 * executeIf, and execute to enter the fail point (as all of these functions have side
 * effects on the counter for times entered).
 */
class WaitForFailPointCommand : public TypedCommand<WaitForFailPointCommand> {
public:
    using Request = WaitForFailPoint;
    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::FailedToParse,
                    "Missing maxTimeMS",
                    request().getGenericArguments().getMaxTimeMS());
            const std::string failPointName = std::string{request().getCommandParameter()};
            FailPoint* failPoint = globalFailPointRegistry().find(failPointName);
            if (failPoint == nullptr)
                uasserted(ErrorCodes::FailPointSetFailed, failPointName + " not found");
            failPoint->waitForTimesEntered(opCtx, request().getTimesEntered());
        }

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        // The command parameter happens to be string so it's historically been interpreted
        // by parseNs as a collection. Continuing to do so here for unexamined compatibility.
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        // No auth needed because it only works when enabled via command line.
        void doCheckAuthorization(OperationContext* opCtx) const override {}
    };

    std::string help() const override {
        return "wait for a fail point to be entered a certain number of times";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool requiresAuth() const override {
        return false;
    }
};

MONGO_REGISTER_COMMAND(WaitForFailPointCommand).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(FaultInjectCmd).testOnly().forRouter().forShard();
}  // namespace mongo
