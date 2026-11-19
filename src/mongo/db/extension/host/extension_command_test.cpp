// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/extension_command.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/extension/host/host_portal.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/command.h"
#include "mongo/db/extension/sdk/command_execution_context_handle.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/tests/shared_test_commands.h"
#include "mongo/db/extension/shared/handle/command/command_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/query/search/search_index_process_interface.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::extension::host {
namespace {

using sdk::shared_test_commands::EchoCommandDescriptor;

const DatabaseName kTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "testDb");
const BSONObj kNoPrivileges = BSON("requiredPrivileges" << BSONArray());

/**
 * Returns the reply that EchoCommandDescriptor gives, without the echoed 'request', for a command
 * on 'coll' in kTestDb that resolves to 'uuid' and 'sourceColl', followed by 'view' if it is not
 * empty.
 */
BSONObj expectedEchoContext(std::string_view coll,
                            std::string_view uuid,
                            std::string_view sourceColl,
                            const BSONObj& view = BSONObj()) {
    BSONObjBuilder bob;
    bob.append("db", kTestDb.toString_forTest());
    bob.append("coll", coll);
    bob.append("uuid", uuid);
    bob.append("source", BSON("db" << kTestDb.toString_forTest() << "coll" << sourceColl));
    if (!view.isEmpty()) {
        bob.append("view", view);
    }
    return bob.obj();
}

/**
 * Returns the 'view' field that EchoCommandDescriptor gives for a view named 'viewName' in kTestDb
 * with the given pipeline.
 */
BSONObj expectedView(std::string_view viewName, const BSONArray& pipeline) {
    return BSON("namespace" << BSON("db" << kTestDb.toString_forTest() << "coll" << viewName)
                            << "pipeline" << pipeline);
}

/**
 * Stands in for the shard or router catalog lookup that the host uses to resolve a command's
 * target. Returns the configured UUID and view, or throws the configured error.
 */
class MockSearchIndexProcessInterface final : public SearchIndexProcessInterface {
public:
    std::pair<boost::optional<UUID>, boost::optional<ResolvedNamespace>>
    fetchCollectionUUIDAndResolveView(OperationContext*,
                                      const NamespaceString& nss,
                                      bool failOnTsColl) override {
        ++calls;
        lastNss = nss;
        // The host must always ask the lookup to reject timeseries targets.
        ASSERT_TRUE(failOnTsColl);
        uassertStatusOK(error);
        return {uuid, view};
    }

    boost::optional<UUID> uuid;
    boost::optional<ResolvedNamespace> view;
    Status error = Status::OK();
    int calls = 0;
    NamespaceString lastNss;
};

/**
 * Reports the command execution context: whether the operation is interrupted and its deadline.
 * A non-OK interrupt status is surfaced as the operation's error.
 */
class ExecCtxCommandDescriptor : public sdk::CommandDescriptor {
public:
    ExecCtxCommandDescriptor(std::string name, BSONObj properties)
        : CommandDescriptor(std::move(name)), _properties(properties.getOwned()) {}

    BSONObj getProperties() const override {
        return _properties;
    }

    BSONObj run(const BSONObj&,
                const ::MongoExtensionCommandContext&,
                const sdk::CommandExecutionContextHandle& execCtx) const override {
        auto interrupted = execCtx->checkForInterrupt();
        if (interrupted.getCode() != MONGO_EXTENSION_STATUS_OK) {
            sdk_uasserted(interrupted.getCode(), std::string(interrupted.getReason()));
        }
        BSONObjBuilder bob;
        bob.append("interrupted", false);
        bob.append("deadline", execCtx->getDeadlineTimestampMs());
        return bob.obj();
    }

private:
    const BSONObj _properties;
};

class ExtensionCommandTest : public ServiceContextTest {
public:
    static constexpr CapabilityVersion kCapabilityVersion = CapabilityVersion::latestCapability();

    void setUp() override {
        ServiceContextTest::setUp();
        sdk::HostServicesAPI::setHostServices(&host_connector::HostServicesAdapter::get(),
                                              kCapabilityVersion);
        auto resolver = std::make_unique<MockSearchIndexProcessInterface>();
        _resolver = resolver.get();
        SearchIndexProcessInterface::set(getServiceContext()->getService(), std::move(resolver));
    }

    MockSearchIndexProcessInterface& resolver() {
        return *_resolver;
    }

    /**
     * Runs 'commandName' against 'coll' in kTestDb, and returns the reply without the echoed
     * 'request'.
     */
    BSONObj runCommandAgainstCollection(std::string_view commandName, std::string_view coll) {
        auto* cmd = findCommand(commandName);
        ASSERT_TRUE(cmd);
        auto opCtx = makeOperationContext();
        BSONObjBuilder result;
        ASSERT_TRUE(cmd->run(opCtx.get(), kTestDb, BSON(commandName << coll), result));
        return result.obj().removeField("request");
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

    /**
     * Registers an ExecCtxCommandDescriptor through the same path the host portal uses.
     */
    void registerExecCtxCommand(std::string name, BSONObj properties = kNoPrivileges) {
        static StaticImmortal<std::vector<std::unique_ptr<sdk::ExtensionCommandDescriptorAdapter>>>
            descriptors;
        auto& descriptor =
            descriptors->emplace_back(std::make_unique<sdk::ExtensionCommandDescriptorAdapter>(
                std::make_unique<ExecCtxCommandDescriptor>(std::move(name),
                                                           std::move(properties))));
        ExtensionCommand::registerCommand(
            CommandDescriptorHandle(descriptor.get(), CapabilityVersion::latestCapability()));
    }

    ExtensionCommand* findCommand(std::string_view name) {
        return dynamic_cast<ExtensionCommand*>(
            getCommandRegistry(ClusterRole::ShardServer)->findCommand(name));
    }

private:
    MockSearchIndexProcessInterface* _resolver = nullptr;
    unittest::ServerParameterGuard _featureFlagExtensionCommands{"featureFlagExtensionCommands",
                                                                 true};
};

TEST_F(ExtensionCommandTest, RunPassesExecutionContextBackedByOperationContext) {
    registerExecCtxCommand("testExtExecCtx");
    auto* cmd = findCommand("testExtExecCtx");
    auto opCtx = makeOperationContext();
    ASSERT_TRUE(cmd);

    BSONObjBuilder result;
    ASSERT_TRUE(cmd->run(opCtx.get(), kTestDb, BSON("testExtExecCtx" << 1), result));
    ASSERT_BSONOBJ_EQ(result.obj(),
                      BSON("interrupted" << false << "deadline" << opCtx->getDeadline().asInt64()));
}

TEST_F(ExtensionCommandTest, RunSeesInterruptFromOperationContext) {
    registerExecCtxCommand("testExtExecCtxKilled");
    auto* cmd = findCommand("testExtExecCtxKilled");
    auto opCtx = makeOperationContext();
    ASSERT_TRUE(cmd);

    opCtx->markKilled(ErrorCodes::Interrupted);

    BSONObjBuilder result;
    ASSERT_THROWS_CODE(cmd->run(opCtx.get(), kTestDb, BSON("testExtExecCtxKilled" << 1), result),
                       DBException,
                       ErrorCodes::Interrupted);
}

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
                                     << "coll"
                                     << "uuid"
                                     << ""
                                     << "source" << BSON("db" << "testDb" << "coll" << "coll")));
}

TEST_F(ExtensionCommandTest, RunPassesMissingCollectionAsEmptyUuid) {
    registerCommand("testExtMissing");

    ASSERT_BSONOBJ_EQ(runCommandAgainstCollection("testExtMissing", "coll"),
                      expectedEchoContext("coll", "", "coll"));
    ASSERT_EQ(resolver().calls, 1);
    ASSERT_EQ(resolver().lastNss, NamespaceString::createNamespaceString_forTest(kTestDb, "coll"));
}

TEST_F(ExtensionCommandTest, RunPassesCollectionUuid) {
    registerCommand("testExtUuid");
    const auto uuid = UUID::gen();
    resolver().uuid = uuid;

    ASSERT_BSONOBJ_EQ(runCommandAgainstCollection("testExtUuid", "coll"),
                      expectedEchoContext("coll", uuid.toString(), "coll"));
}

TEST_F(ExtensionCommandTest, RunPassesView) {
    registerCommand("testExtView");
    const auto uuid = UUID::gen();
    const auto pipeline =
        std::vector<BSONObj>{BSON("$match" << BSON("a" << 1)), BSON("$project" << BSON("b" << 0))};
    resolver().uuid = uuid;
    resolver().view.emplace(NamespaceString::createNamespaceString_forTest(kTestDb, "view"),
                            NamespaceString::createNamespaceString_forTest(kTestDb, "coll"),
                            pipeline,
                            BSONObj());

    ASSERT_BSONOBJ_EQ(
        runCommandAgainstCollection("testExtView", "view"),
        expectedEchoContext("view",
                            uuid.toString(),
                            "coll",
                            expectedView("view", BSON_ARRAY(pipeline[0] << pipeline[1]))));
}

TEST_F(ExtensionCommandTest, RunPassesViewWithMissingSourceCollection) {
    registerCommand("testExtViewNoSource");
    resolver().view.emplace(NamespaceString::createNamespaceString_forTest(kTestDb, "view"),
                            NamespaceString::createNamespaceString_forTest(kTestDb, "coll"),
                            std::vector<BSONObj>{},
                            BSONObj());

    ASSERT_BSONOBJ_EQ(runCommandAgainstCollection("testExtViewNoSource", "view"),
                      expectedEchoContext("view", "", "coll", expectedView("view", BSONArray())));
}

// A router throws NamespaceNotFound when the database does not exist, whereas a shard returns no
// UUID. The extension sees the same context either way.
TEST_F(ExtensionCommandTest, RunPassesThrownNamespaceNotFoundAsEmptyUuid) {
    registerCommand("testExtNoDb");
    resolver().error = {ErrorCodes::NamespaceNotFound, "database not found"};

    ASSERT_BSONOBJ_EQ(runCommandAgainstCollection("testExtNoDb", "coll"),
                      expectedEchoContext("coll", "", "coll"));
}

TEST_F(ExtensionCommandTest, RunReturnsOtherResolutionErrorsWithoutRunning) {
    registerCommand("testExtResolveError");
    auto* cmd = findCommand("testExtResolveError");
    ASSERT_TRUE(cmd);
    auto opCtx = makeOperationContext();

    // Stands in for the timeseries rejection, or any other error from the catalog lookup.
    resolver().error = {ErrorCodes::InternalError, "lookup failed"};
    BSONObjBuilder result;
    ASSERT_THROWS_CODE(
        cmd->run(opCtx.get(), kTestDb, BSON("testExtResolveError" << "coll"), result),
        DBException,
        ErrorCodes::InternalError);
    // The extension command did not run, so nothing was appended.
    ASSERT_BSONOBJ_EQ(result.obj(), BSONObj());
}

TEST_F(ExtensionCommandTest, RunDoesNotResolveWithoutCollection) {
    registerCommand("testExtNoColl");
    resolver().error = {ErrorCodes::InternalError, "should not be called"};

    ASSERT_BSONOBJ_EQ(runCommandAgainstCollection("testExtNoColl", ""),
                      expectedEchoContext("", "", ""));
    ASSERT_EQ(resolver().calls, 0);
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
