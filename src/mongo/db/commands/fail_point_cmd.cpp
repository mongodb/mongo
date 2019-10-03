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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {

/**
 * Command for modifying installed fail points.
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
    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {}

    std::string help() const override {
        return "modifies the settings of a fail point";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const std::string failPointName(cmdObj.firstElement().str());
        setGlobalFailPoint(failPointName, cmdObj);

        return true;
    }
};
MONGO_REGISTER_TEST_COMMAND(FaultInjectCmd);
}  // namespace mongo
