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
    void setUp() override {
        setNegotiatedCapabilityVersionForTest(CapabilityVersion::zeroCapability());
    }

    /**
     * Invokes get_mongodb_extension with a real (non-null) HostServices pointer and returns the
     * resulting status. Ownership of the status is transferred to the caller.
     */
    ::MongoExtensionStatus* callGetMongoExtension(::MongoExtensionAPIVersion version) {
        // Deliberately not derived from 'version': these tests feed in versions the extension is
        // expected to reject, and getHostServices only vends instances for negotiable versions.
        return get_mongodb_extension(
            version,
            host_connector::getHostServices(CapabilityVersion::legacyCapability()),
            &_extension);
    }

    /**
     * A rejected version must leave the extension unset and - most importantly - return normally.
     * These versions are all outside the bounds of every MongoExtensionStatus implementation, so
     * no status the host would understand can be allocated; rejection surfaces as a null status,
     * which load_extension.cpp reports as a load failure.
     */
    void assertRejection(::MongoExtensionAPIVersion version) {
        auto* status = callGetMongoExtension(version);
        ASSERT_EQ(status, nullptr);
        ASSERT_EQ(_extension, nullptr);
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
