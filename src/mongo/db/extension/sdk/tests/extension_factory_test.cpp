// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/extension_factory.h"

#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/unittest/unittest.h"

/**
 * Test extension registered so that DEFINE_GET_EXTENSION() has a version to publish. The
 * registration and entry-point definitions live at global scope, mirroring real extensions.
 */
class ExtensionFactoryTestExtension : public mongo::extension::sdk::Extension {
public:
    void initialize(const mongo::extension::sdk::HostPortalHandle& portal) override {
        // Intentionally left blank.
    }
};

REGISTER_EXTENSION(ExtensionFactoryTestExtension)
DEFINE_GET_EXTENSION()

namespace mongo::extension::sdk {
namespace {

class GetExtensionVersionRejectionTest : public unittest::Test {
protected:
    /**
     * Invokes get_mongodb_extension with a real (non-null) HostServices pointer and returns the
     * resulting status. Ownership of the status is transferred to the caller.
     */
    ::MongoExtensionStatus* callGetMongoExtension(::MongoExtensionAPIVersion version) {
        return get_mongodb_extension(
            version, &host_connector::HostServicesAdapter::get(), &_extension);
    }

    /**
     * A rejected version must produce an error status, leave the extension unset, and - most
     * importantly - return normally. Installing a HostServices handle with an unsupported
     * capability version throws inside the noexcept setHostServices(), so a rejection that
     * installs the handle first terminates the process instead of returning.
     */
    void assertRejection(::MongoExtensionAPIVersion version) {
        auto* status = callGetMongoExtension(version);
        ASSERT_NE(status, nullptr);
        ASSERT_EQ(status->vtable->get_code(status), 13693502);
        ASSERT_EQ(_extension, nullptr);

        const std::string reason{byteViewAsStringView(status->vtable->get_reason(status))};
        ASSERT_NE(reason.find("outside the extension's supported capability bounds"),
                  std::string::npos);

        status->vtable->destroy(status);
    }

    const ::MongoExtension* _extension = nullptr;
};

TEST_F(GetExtensionVersionRejectionTest, OutOfBoundsMajorVersionReturnsError) {
    assertRejection(::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_MAJOR_VERSION + 1,
                                               MONGODB_EXTENSION_API_MINOR_VERSION});
}

TEST_F(GetExtensionVersionRejectionTest, OutOfBoundsMinorVersionReturnsError) {
    assertRejection(::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_MAJOR_VERSION,
                                               MONGODB_EXTENSION_API_MINOR_VERSION + 1});
}

TEST_F(GetExtensionVersionRejectionTest, BelowLegacyVersionReturnsError) {
    assertRejection(::MongoExtensionAPIVersion{0, 9});
}

// A minor version exceeding the packed width cannot be represented as a CapabilityVersion at
// all; rejection must still return an error status rather than throw across the C boundary.
TEST_F(GetExtensionVersionRejectionTest, UnencodableMinorVersionReturnsError) {
    assertRejection(::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_MAJOR_VERSION,
                                               CapabilityVersion::kCapabilityMask + 1});
}

}  // namespace
}  // namespace mongo::extension::sdk
