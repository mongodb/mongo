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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <set>
#include <string>

namespace mongo {

const std::set<std::string>& kApiVersion2 = {"2"};           // Test-only.
const std::set<std::string>& kApiVersion1And2 = {"1", "2"};  // Test-only.

class TestVersion2Cmd : public BasicCommand {
public:
    TestVersion2Cmd() : BasicCommand("testVersion2") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersion2;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();  // No auth required
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto apiParameters = APIParameters::get(opCtx);
        apiParameters.appendInfo(&result);
        return true;
    }
};

class TestVersions1And2Cmd : public BasicCommand {
public:
    TestVersions1And2Cmd() : BasicCommand("testVersions1And2") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersion1And2;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();  // No auth required
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto apiParameters = APIParameters::get(opCtx);
        apiParameters.appendInfo(&result);
        return true;
    }
};

class TestDeprecationInVersion2Cmd : public BasicCommand {
public:
    TestDeprecationInVersion2Cmd() : BasicCommand("testDeprecationInVersion2") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersion1And2;
    }

    const std::set<std::string>& deprecatedApiVersions() const override {
        return kApiVersion2;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();  // No auth required
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto apiParameters = APIParameters::get(opCtx);
        apiParameters.appendInfo(&result);
        return true;
    }
};

class TestRemovalCmd : public BasicCommand {
public:
    TestRemovalCmd() : BasicCommand("testRemoval") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext*,
                                 const DatabaseName&,
                                 const BSONObj&) const override {
        return Status::OK();  // No auth required
    }

    bool run(OperationContext* opCtx,
             const DatabaseName&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto apiParameters = APIParameters::get(opCtx);
        apiParameters.appendInfo(&result);
        return true;
    }
};

MONGO_REGISTER_COMMAND(TestVersion2Cmd).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestVersions1And2Cmd).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestDeprecationInVersion2Cmd).testOnly().forRouter().forShard();
MONGO_REGISTER_COMMAND(TestRemovalCmd).testOnly().forRouter().forShard();

}  // namespace mongo
