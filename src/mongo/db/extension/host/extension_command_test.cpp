// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_command.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/command.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/tests/shared_test_commands.h"
#include "mongo/db/extension/shared/handle/command/command_descriptor.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/static_immortal.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mongo::extension::host {
namespace {

using sdk::shared_test_commands::EchoCommandDescriptor;

const DatabaseName kTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "testDb");

class ExtensionCommandTest : public ServiceContextTest {
public:
    static constexpr CapabilityVersion kCapabilityVersion = CapabilityVersion::latestCapability();

    void setUp() override {
        ServiceContextTest::setUp();
        sdk::HostServicesAPI::setHostServices(&host_connector::HostServicesAdapter::get(),
                                              kCapabilityVersion);
    }

    /**
     * Registers a command through the same path the host portal uses at extension load. Registered
     * commands keep an unowned handle to the descriptor for the rest of the process, so every
     * descriptor is kept alive for the process lifetime, including ones whose registration was
     * skipped or rejected.
     */
    void registerCommand(std::string name,
                         BSONObj properties = EchoCommandDescriptor::defaultProperties()) {
        static StaticImmortal<std::vector<std::unique_ptr<sdk::ExtensionCommandDescriptorAdapter>>>
            descriptors;
        auto& descriptor =
            descriptors->emplace_back(std::make_unique<sdk::ExtensionCommandDescriptorAdapter>(
                std::make_unique<EchoCommandDescriptor>(std::move(name), std::move(properties))));
        ExtensionCommand::registerCommand(
            CommandDescriptorHandle(descriptor.get(), kCapabilityVersion));
    }

    ExtensionCommand* findCommand(std::string_view name) {
        return dynamic_cast<ExtensionCommand*>(
            getCommandRegistry(ClusterRole::ShardServer)->findCommand(name));
    }

private:
    unittest::ServerParameterGuard _featureFlagExtensionCommands{"featureFlagExtensionCommands",
                                                                 true};
};

TEST_F(ExtensionCommandTest, PropertiesMapToCommandOverrides) {
    registerCommand("testExtProperties",
                    BSON("readWriteType" << "write"
                                         << "allowedOnSecondary"
                                         << "optIn"
                                         << "adminOnly" << true << "supportsWriteConcern" << true
                                         << "isSubjectToIngressAdmissionControl" << true
                                         << "requiredPrivileges" << BSONArray()));

    auto* cmd = findCommand("testExtProperties");
    ASSERT_TRUE(cmd);
    ASSERT_EQ(cmd->secondaryAllowed(getServiceContext()), Command::AllowedOnSecondary::kOptIn);
    ASSERT_EQ(cmd->getReadWriteType(), Command::ReadWriteType::kWrite);
    ASSERT_TRUE(cmd->adminOnly());
    ASSERT_TRUE(cmd->supportsWriteConcern(BSONObj()));
    ASSERT_TRUE(cmd->isSubjectToIngressAdmissionControl());
}

TEST_F(ExtensionCommandTest, RunPassesNamespaceToExtension) {
    registerCommand("testExtRun");
    auto* cmd = findCommand("testExtRun");
    auto opCtx = makeOperationContext();

    ASSERT_TRUE(cmd);

    BSONObjBuilder result;
    const auto request = BSON("testExtRun" << "coll");
    ASSERT_TRUE(cmd->run(opCtx.get(), kTestDb, request, result));
    ASSERT_BSONOBJ_EQ(result.obj(),
                      BSON("request" << request << "db"
                                     << "testDb"
                                     << "coll"
                                     << "coll"));
}

TEST_F(ExtensionCommandTest, RunRejectsReservedReplyFields) {
    registerCommand("testExtReserved");
    auto* cmd = findCommand("testExtReserved");
    auto opCtx = makeOperationContext();
    ASSERT_TRUE(cmd);

    for (auto&& extra : {BSON("ok" << 0), BSON("errmsg" << "x"), BSON("operationTime" << 1)}) {
        BSONObjBuilder result;
        ASSERT_THROWS_CODE(
            cmd->run(
                opCtx.get(), kTestDb, BSON("testExtReserved" << 1 << "extra" << extra), result),
            DBException,
            ErrorCodes::ExtensionError);
    }
}

// The test fixture's process serves only the shard role.
TEST_F(ExtensionCommandTest, RegisterCommandOnlyAddsItToServedRoles) {
    registerCommand("testExtRouterOnly",
                    BSON("clusterRole" << "router"
                                       << "requiredPrivileges" << BSONArray()));
    ASSERT_FALSE(findCommand("testExtRouterOnly"));
}

TEST_F(ExtensionCommandTest, RegisterCommandRejectsDuplicateName) {
    registerCommand("testExtDup");
    ASSERT_THROWS_CODE(registerCommand("testExtDup"), DBException, ErrorCodes::ExtensionError);
}

TEST_F(ExtensionCommandTest, RegisterCommandRejectsInvalidName) {
    for (auto name : {"", "$testExt", "test.ext"}) {
        ASSERT_THROWS_CODE(registerCommand(name), DBException, ErrorCodes::ExtensionError);
    }
    ASSERT_THROWS_CODE(
        registerCommand(std::string("test\0ext", 8)), DBException, ErrorCodes::ExtensionError);
}

TEST_F(ExtensionCommandTest, RegisterCommandRejectsInvalidProperties) {
    // requiredPrivileges is a required field.
    ASSERT_THROWS_CODE(
        registerCommand("testExtInvalid", BSONObj()), DBException, ErrorCodes::IDLFailedToParse);
    ASSERT_FALSE(findCommand("testExtInvalid"));
}

TEST_F(ExtensionCommandTest, RegisterCommandIsSkippedWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard flag{"featureFlagExtensionCommands", false};
    registerCommand("testExtFlagOff");
    ASSERT_FALSE(findCommand("testExtFlagOff"));
}

TEST_F(ExtensionCommandTest, HostPortalRefusesCommandFromApi10Session) {
    HostPortal portal(CapabilityVersion(MONGODB_EXTENSION_API_MAJOR_VERSION, 0));
    sdk::ExtensionCommandDescriptorAdapter descriptor(
        std::make_unique<EchoCommandDescriptor>("testExtApi10"));

    ASSERT_THROWS_CODE(
        portal.registerCommandDescriptor(&descriptor), DBException, ErrorCodes::ExtensionError);
    ASSERT_FALSE(findCommand("testExtApi10"));
}

}  // namespace
}  // namespace mongo::extension::host
