// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
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

    bool requiresAuthzChecks() const override {
        return false;
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

    bool requiresAuthzChecks() const override {
        return false;
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

    bool requiresAuthzChecks() const override {
        return false;
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

    bool requiresAuthzChecks() const override {
        return false;
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
