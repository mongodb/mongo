// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/command.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/public/api.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/command/command_descriptor.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo::extension::sdk {
namespace {

/**
 * Echoes the request back, along with the namespace it received in the command context.
 */
class EchoCommandDescriptor : public CommandDescriptor {
public:
    static constexpr std::string_view kCommandName = "testEcho";

    EchoCommandDescriptor() : CommandDescriptor(std::string(kCommandName)) {}

    BSONObj getProperties() const override {
        return BSON("clusterRole" << "both"
                                  << "adminOnly" << false);
    }

    BSONObj run(const BSONObj& requestBson,
                const ::MongoExtensionCommandContext& ctx) const override {
        return BSON("request" << requestBson << "db"
                              << byteViewAsStringView(ctx.namespaceString.databaseName) << "coll"
                              << byteViewAsStringView(ctx.namespaceString.collectionName));
    }
};

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

    BSONObj run(const BSONObj&, const ::MongoExtensionCommandContext&) const override {
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
        return &ExtensionStatusOK::getInstance();
    }

    static ::MongoExtensionStatus* _extRun(const ::MongoExtensionCommandDescriptor*,
                                           ::MongoExtensionByteView,
                                           const ::MongoExtensionCommandContext*,
                                           ::MongoExtensionByteBuf** responseBson) noexcept {
        *responseBson = new CountingByteBuf(BSON("ok" << 1));
        return &ExtensionStatusOK::getInstance();
    }

    static constexpr ::MongoExtensionCommandDescriptorVTable VTABLE = {
        .get_name = &_extGetName,
        .get_properties = &_extGetProperties,
        .run = &_extRun,
    };
};

class CommandDescriptorTest : public unittest::Test {
public:
    void setUp() override {
        // Initialize HostServices so that the SDK assertion helpers can reach the host.
        HostServicesAPI::setHostServices(&host_connector::HostServicesAdapter::get(),
                                         CapabilityVersion::legacyCapability());
    }

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
    const ::MongoExtensionCommandContext ctx{
        .namespaceString = {.databaseName = stringViewAsByteView("testDb"),
                            .collectionName = stringViewAsByteView("coll")}};

    BSONObjBuilder result;
    handle->run(request, ctx, result);

    ASSERT_BSONOBJ_EQ(result.obj(),
                      BSON("request" << request << "db"
                                     << "testDb"
                                     << "coll"
                                     << "coll"));
}

TEST_F(CommandDescriptorTest, RunAppendsToExistingResult) {
    auto adapter = makeAdapter<EchoCommandDescriptor>();
    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};

    const auto request = BSON("testEcho" << 1);
    const ::MongoExtensionCommandContext ctx{
        .namespaceString = {.databaseName = stringViewAsByteView("testDb"),
                            .collectionName = stringViewAsByteView("")}};

    BSONObjBuilder result;
    result.append("existing", true);
    handle->run(request, ctx, result);

    ASSERT_BSONOBJ_EQ(result.obj(),
                      BSON("existing" << true << "request" << request << "db"
                                      << "testDb"
                                      << "coll"
                                      << ""));
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
    StatusHandle status(adapter->vtable->run(adapter.get(), objAsByteView(request), &ctx, &buf));
    ASSERT_EQ(status->getCode(), 13729903);
    ASSERT_EQ(buf, nullptr);

    CommandDescriptorHandle handle{adapter.get(), CapabilityVersion::latestCapability()};
    BSONObjBuilder result;
    result.append("existing", true);
    ASSERT_THROWS_CODE(handle->run(request, ctx, result), DBException, 13729903);

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
    handle->run(BSON("testCounting" << 1), ctx, result);
    ASSERT_EQ(CountingByteBuf::destroyed, 2);
    ASSERT_BSONOBJ_EQ(result.obj(), BSON("ok" << 1));
}

}  // namespace
}  // namespace mongo::extension::sdk
