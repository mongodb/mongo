// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/command.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host_connector/adapter/host_portal_adapter.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/tests/sdk_test.h"
#include "mongo/db/extension/sdk/tests/shared_test_commands.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/command/command_descriptor.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace mongo::extension::sdk {
namespace {

using shared_test_commands::EchoCommandDescriptor;

/**
 * A minimal command execution context implemented directly against the C API, used to exercise the
 * SDK handle. checkForInterrupt() reports a fixed code and the deadline is a fixed value.
 */
struct TestCommandExecutionContext final : public ::MongoExtensionCommandExecutionContext {
    TestCommandExecutionContext(int32_t interruptCode = MONGO_EXTENSION_STATUS_OK,
                                int64_t deadlineMs = std::numeric_limits<int64_t>::max())
        : ::MongoExtensionCommandExecutionContext{&VTABLE},
          interruptCode(interruptCode),
          deadlineMs(deadlineMs) {}

    static ::MongoExtensionStatus* _extCheckForInterrupt(
        const ::MongoExtensionCommandExecutionContext* ctx,
        ::MongoExtensionStatus* opStatus) noexcept {
        const auto* self = static_cast<const TestCommandExecutionContext*>(ctx);
        if (self->interruptCode != MONGO_EXTENSION_STATUS_OK) {
            auto statusAPI = StatusAPI(opStatus, CapabilityVersion::latestCapability());
            statusAPI.setCode(self->interruptCode);
            statusAPI.setReason("interrupted");
        }
        return getExtensionStatusOK(CapabilityVersion::latestCapability());
    }

    static ::MongoExtensionStatus* _extGetDeadlineTimestampMs(
        const ::MongoExtensionCommandExecutionContext* ctx, int64_t* deadlineTimestampMs) noexcept {
        *deadlineTimestampMs = static_cast<const TestCommandExecutionContext*>(ctx)->deadlineMs;
        return getExtensionStatusOK(CapabilityVersion::latestCapability());
    }

    static constexpr ::MongoExtensionCommandExecutionContextVTable VTABLE = {
        .check_for_interrupt = &_extCheckForInterrupt,
        .get_deadline_timestamp_ms = &_extGetDeadlineTimestampMs,
    };

    int32_t interruptCode;
    int64_t deadlineMs;
};

// A never-interrupted execution context with no deadline, used by tests that do not care about it.
TestCommandExecutionContext kNoopExecCtx;

/**
 * Throws from both getProperties() and run().
 */
class ThrowingCommandDescriptor : public CommandDescriptor {
public:
    ThrowingCommandDescriptor() : CommandDescriptor("testThrowing") {}

    BSONObj getProperties() const override {
        sdk_uasserted(13729902, "getProperties failed");
        return {};
    }

    BSONObj run(const BSONObj&,
                const ::MongoExtensionCommandContext&,
                const CommandExecutionContextHandle&) const override {
        sdk_uasserted(13729903, "run failed");
        return {};
    }
};

/**
 * A ByteBuf that counts how many times it is destroyed, used to verify that the handle takes
 * ownership of the buffers returned across the boundary.
 */
class CountingByteBuf final : public ::MongoExtensionByteBuf {
public:
    static inline int destroyed = 0;

    explicit CountingByteBuf(BSONObj obj)
        : ::MongoExtensionByteBuf{&VTABLE}, _obj(obj.getOwned()) {}

private:
    static void _extDestroy(::MongoExtensionByteBuf* buf) noexcept {
        ++destroyed;
        delete static_cast<CountingByteBuf*>(buf);
    }

    static ::MongoExtensionByteView _extGetView(const ::MongoExtensionByteBuf* buf) noexcept {
        return objAsByteView(static_cast<const CountingByteBuf*>(buf)->_obj);
    }

    static constexpr ::MongoExtensionByteBufVTable VTABLE = {
        .destroy = &_extDestroy,
        .get_view = &_extGetView,
    };

    BSONObj _obj;
};

/**
 * A raw C-API command descriptor (no SDK adapter) that returns CountingByteBufs.
 */
struct CountingCommandDescriptor : public ::MongoExtensionCommandDescriptor {
    CountingCommandDescriptor() : ::MongoExtensionCommandDescriptor{&VTABLE} {}

    static ::MongoExtensionByteView _extGetName(const ::MongoExtensionCommandDescriptor*) noexcept {
        return stringViewAsByteView("testCounting");
    }

    static ::MongoExtensionStatus* _extGetProperties(
        const ::MongoExtensionCommandDescriptor*, ::MongoExtensionByteBuf** properties) noexcept {
        *properties = new CountingByteBuf(BSON("clusterRole" << "shard"));
        return getExtensionStatusOK(CapabilityVersion::latestCapability());
    }

    static ::MongoExtensionStatus* _extRun(const ::MongoExtensionCommandDescriptor*,
                                           ::MongoExtensionByteView,
                                           const ::MongoExtensionCommandContext*,
                                           const ::MongoExtensionCommandExecutionContext*,
                                           ::MongoExtensionByteBuf** responseBson) noexcept {
        *responseBson = new CountingByteBuf(BSON("ok" << 1));
        return getExtensionStatusOK(CapabilityVersion::latestCapability());
    }

    static constexpr ::MongoExtensionCommandDescriptorVTable VTABLE = {
        .get_name = &_extGetName,
        .get_properties = &_extGetProperties,
        .run = &_extRun,
    };
};

class CommandDescriptorTest : public sdk::SdkTest {
public:
    template <class Descriptor>
    static std::unique_ptr<ExtensionCommandDescriptorAdapter> makeAdapter() {
        return std::make_unique<ExtensionCommandDescriptorAdapter>(std::make_unique<Descriptor>());
    }
};

TEST_F(CommandDescriptorTest, HandleRequiresCapability11) {
    ASSERT_FALSE(VTableAPI<::MongoExtensionCommandDescriptor>::isSupported(
        CapabilityVersion::legacyCapability()));
    ASSERT_TRUE(VTableAPI<::MongoExtensionCommandDescriptor>::isSupported(CapabilityVersion(1, 1)));
}

TEST_F(CommandDescriptorTest, ExecutionContextHandleRequiresCapability11) {
    ASSERT_FALSE(VTableAPI<::MongoExtensionCommandExecutionContext>::isSupported(
        CapabilityVersion::legacyCapability()));
    ASSERT_TRUE(
        VTableAPI<::MongoExtensionCommandExecutionContext>::isSupported(CapabilityVersion(1, 1)));
}

TEST_F(CommandDescriptorTest, ExecutionContextHandleRoundTrips) {
    TestCommandExecutionContext execCtx(/*interruptCode=*/11601, /*deadlineMs=*/12345);
    CommandExecutionContextHandle handle{&execCtx, CapabilityVersion::latestCapability()};

    ASSERT_EQ(handle->getDeadlineTimestampMs(), 12345);

    auto status = handle->checkForInterrupt();
    ASSERT_EQ(status.getCode(), 11601);
    ASSERT_EQ(status.getReason(), "interrupted");
}

TEST_F(CommandDescriptorTest, ExecutionContextHandleReportsOkWhenNotInterrupted) {
    TestCommandExecutionContext execCtx;
    CommandExecutionContextHandle handle{&execCtx, CapabilityVersion::latestCapability()};

    ASSERT_EQ(handle->getDeadlineTimestampMs(), std::numeric_limits<int64_t>::max());
    ASSERT_EQ(handle->checkForInterrupt().getCode(), MONGO_EXTENSION_STATUS_OK);
}

TEST_F(CommandDescriptorTest, GetNameRoundTrips) {
    auto adapter = makeAdapter<EchoCommandDescriptor>();
    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};

    ASSERT_EQ(handle->getName(), EchoCommandDescriptor::kCommandName);
}

TEST_F(CommandDescriptorTest, GetPropertiesRoundTrips) {
    auto adapter = makeAdapter<EchoCommandDescriptor>();
    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};

    auto properties = handle->getProperties();
    ASSERT_BSONOBJ_EQ(properties, EchoCommandDescriptor().getProperties());
    ASSERT_TRUE(properties.isOwned());
}

TEST_F(CommandDescriptorTest, RunRoundTripsRequestAndContext) {
    auto adapter = makeAdapter<EchoCommandDescriptor>();
    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};

    const auto request = BSON("testEcho" << "coll" << "payload" << BSON("a" << 1));
    const ::MongoExtensionNamespaceString nss{.databaseName = stringViewAsByteView("testDb"),
                                              .collectionName = stringViewAsByteView("coll")};
    const ::MongoExtensionCommandContext ctx{.namespaceString = nss, .sourceNamespaceString = nss};

    BSONObjBuilder result;
    handle->run(request, ctx, kNoopExecCtx, result);

    const auto nssBson = shared_test_commands::namespaceToBSON(nss);
    ASSERT_BSONOBJ_EQ(result.obj(),
                      BSONObjBuilder()
                          .append("request", request)
                          .appendElements(nssBson)
                          .append("uuid", "")
                          .append("source", nssBson)
                          .obj());
}

TEST_F(CommandDescriptorTest, RunPassesResolvedContextToExtension) {
    auto adapter = makeAdapter<EchoCommandDescriptor>();
    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};

    const auto stage = BSON("$match" << BSON("a" << 1));
    const ::MongoExtensionByteView viewPipeline[] = {objAsByteView(stage)};
    const ::MongoExtensionNamespaceString viewNss{.databaseName = stringViewAsByteView("testDb"),
                                                  .collectionName = stringViewAsByteView("view")};
    const ::MongoExtensionNamespaceString sourceNss{.databaseName = stringViewAsByteView("testDb"),
                                                    .collectionName = stringViewAsByteView("coll")};
    const ::MongoExtensionResolvedNamespace view{
        .viewNamespace = viewNss,
        .viewPipelineLen = 1,
        .viewPipeline = viewPipeline,
    };
    const ::MongoExtensionCommandContext ctx{
        .namespaceString = viewNss,
        .uuidString = stringViewAsByteView("some-uuid"),
        .resolvedNamespace = &view,
        .sourceNamespaceString = sourceNss,
    };

    const auto request = BSON("testEcho" << "view");
    BSONObjBuilder result;
    handle->run(request, ctx, kNoopExecCtx, result);

    const auto viewBson = shared_test_commands::namespaceToBSON(viewNss);
    ASSERT_BSONOBJ_EQ(
        result.obj(),
        BSONObjBuilder()
            .append("request", request)
            .appendElements(viewBson)
            .append("uuid", "some-uuid")
            .append("source", shared_test_commands::namespaceToBSON(sourceNss))
            .append("view", BSON("namespace" << viewBson << "pipeline" << BSON_ARRAY(stage)))
            .obj());
}

TEST_F(CommandDescriptorTest, RunAppendsToExistingResult) {
    auto adapter = makeAdapter<EchoCommandDescriptor>();
    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};

    const auto request = BSON("testEcho" << 1);
    const ::MongoExtensionCommandContext ctx{
        .namespaceString = {.databaseName = stringViewAsByteView("testDb"),
                            .collectionName = stringViewAsByteView("")},
        .sourceNamespaceString = {.databaseName = stringViewAsByteView("testDb"),
                                  .collectionName = stringViewAsByteView("")}};

    BSONObjBuilder result;
    result.append("existing", true);
    handle->run(request, ctx, kNoopExecCtx, result);

    ASSERT_BSONOBJ_EQ(result.obj(),
                      BSON("existing" << true << "request" << request << "db"
                                      << "testDb"
                                      << "coll"
                                      << ""
                                      << "uuid"
                                      << ""
                                      << "source" << BSON("db" << "testDb" << "coll" << "")));
}

TEST_F(CommandDescriptorTest, GetPropertiesExceptionIsReturnedAsStatus) {
    auto adapter = makeAdapter<ThrowingCommandDescriptor>();

    // Calling through the vtable directly shows the exception crosses the boundary as a status.
    ::MongoExtensionByteBuf* buf{nullptr};
    StatusHandle status(adapter->vtable->get_properties(adapter.get(), &buf));
    ASSERT_EQ(status->getCode(), 13729902);
    ASSERT_EQ(buf, nullptr);

    // The handle converts the status back into an exception.
    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};
    ASSERT_THROWS_CODE(handle->getProperties(), DBException, 13729902);
}

TEST_F(CommandDescriptorTest, RunExceptionIsReturnedAsStatus) {
    auto adapter = makeAdapter<ThrowingCommandDescriptor>();
    const auto request = BSON("testThrowing" << 1);
    const ::MongoExtensionCommandContext ctx{
        .namespaceString = {.databaseName = stringViewAsByteView("testDb"),
                            .collectionName = stringViewAsByteView("coll")}};

    ::MongoExtensionByteBuf* buf{nullptr};
    StatusHandle status(
        adapter->vtable->run(adapter.get(), objAsByteView(request), &ctx, &kNoopExecCtx, &buf));
    ASSERT_EQ(status->getCode(), 13729903);
    ASSERT_EQ(buf, nullptr);

    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};
    BSONObjBuilder result;
    result.append("existing", true);
    ASSERT_THROWS_CODE(handle->run(request, ctx, kNoopExecCtx, result), DBException, 13729903);

    // Nothing from the failed run is appended to the result.
    ASSERT_BSONOBJ_EQ(result.obj(), BSON("existing" << true));
}

TEST_F(CommandDescriptorTest, HandleTakesOwnershipOfReturnedBuffers) {
    CountingCommandDescriptor descriptor;
    CommandDescriptorHandle handle{&descriptor, CapabilityVersion::latestCapability()};
    CountingByteBuf::destroyed = 0;

    auto properties = handle->getProperties();
    ASSERT_EQ(CountingByteBuf::destroyed, 1);
    // The returned properties outlive the buffer they were read from.
    ASSERT_BSONOBJ_EQ(properties, BSON("clusterRole" << "shard"));

    BSONObjBuilder result;
    const ::MongoExtensionCommandContext ctx{
        .namespaceString = {.databaseName = stringViewAsByteView("testDb"),
                            .collectionName = stringViewAsByteView("coll")}};
    handle->run(BSON("testCounting" << 1), ctx, kNoopExecCtx, result);
    ASSERT_EQ(CountingByteBuf::destroyed, 2);
    ASSERT_BSONOBJ_EQ(result.obj(), BSON("ok" << 1));
}

TEST_F(CommandDescriptorTest, RegisterCommandDescriptorIsRefusedAtApi10) {
    // Stands in for a 1.0 host's vtable, which does not contain register_command_descriptor.
    auto vtable = host_connector::HostPortalAdapter::getVTable();
    vtable.register_command_descriptor = nullptr;
    ::MongoExtensionHostPortal portal{&vtable, {}, 0};

    // Building the handle checks the vtable, which must not require the 1.1 slot at 1.0. The slot
    // is null, so reaching it would crash rather than throw.
    HostPortalHandle handle(&portal, CapabilityVersion(MONGODB_EXTENSION_API_MAJOR_VERSION, 0));
    auto adapter = makeAdapter<EchoCommandDescriptor>();
    ASSERT_THROWS_CODE(handle->registerCommandDescriptor(adapter.get()), DBException, 13730000);
}

/**
 * Registers EchoCommandDescriptor twice.
 */
class DuplicateCommandExtension : public Extension {
public:
    void initialize(const HostPortalHandle& portal) override {
        _registerCommand<EchoCommandDescriptor>(portal);
        _registerCommand<EchoCommandDescriptor>(portal);
    }
};

TEST_F(CommandDescriptorTest, RegisterCommandRejectsDuplicateInSameExtension) {
    // Accepts every command, so that only the SDK's own duplicate check can reject the second.
    auto vtable = host_connector::HostPortalAdapter::getVTable();
    vtable.register_command_descriptor =
        [](const ::MongoExtensionHostPortal*,
           const ::MongoExtensionCommandDescriptor*) -> ::MongoExtensionStatus* {
        return getExtensionStatusOK(CapabilityVersion::latestCapability());
    };
    ::MongoExtensionHostPortal portal{&vtable, {}, 0};

    HostPortalHandle handle(&portal, CapabilityVersion::latestCapability());
    DuplicateCommandExtension extension;
    ASSERT_THROWS_CODE(extension.initialize(handle), DBException, 13730002);
}

}  // namespace
}  // namespace mongo::extension::sdk
