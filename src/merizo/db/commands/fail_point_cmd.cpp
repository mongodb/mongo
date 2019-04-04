/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kCommand

#include <vector>

#include "merizo/base/init.h"
#include "merizo/db/auth/action_set.h"
#include "merizo/db/auth/action_type.h"
#include "merizo/db/auth/privilege.h"
#include "merizo/db/commands.h"
#include "merizo/db/commands/test_commands_enabled.h"
#include "merizo/util/fail_point_service.h"
#include "merizo/util/log.h"

namespace merizo {

/**
 * Command for modifying installed fail points.
 *
 * Format
 * {
 *    configureFailPoint: <string>, // name of the fail point.
 *    mode: <string|Object>, // the new mode to set. Can have one of the
 *        following format:
 *
 *        1. 'off' - disable fail point.
 *        2. 'alwaysOn' - fail point is always active.
 *        3. { activationProbability: <n> } - n should be a double between 0 and 1,
 *           representing the probability that the fail point will fire.  0 means never,
 *           1 means (nearly) always.
 *        4. { times: <n> } - n should be positive and within the range of a 32 bit
 *            signed integer and this is the number of passes on the fail point will
 *            remain activated.
 *
 *    data: <Object> // optional arbitrary object to store.
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
}
