/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/initialize_api_parameters.h"

namespace mongo {

/**
 * Command for testing API Version deprecation logic. The command replies with the values of the
 * OperationContext's API parameters.
 */
class TestDeprecationCmd : public BasicCommand {
public:
    TestDeprecationCmd() : BasicCommand("testDeprecation") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    const std::set<std::string>& deprecatedApiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool requiresAuth() const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {}

    std::string help() const override {
        return "replies with the values of the OperationContext's API parameters";
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        result.append("apiVersion", APIParameters::get(opCtx).getAPIVersion());
        result.append("apiStrict", APIParameters::get(opCtx).getAPIStrict());
        result.append("apiDeprecationErrors", APIParameters::get(opCtx).getAPIDeprecationErrors());
        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(TestDeprecationCmd);


}  // namespace mongo