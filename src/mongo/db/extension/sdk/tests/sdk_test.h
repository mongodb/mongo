// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/negotiated_capability.h"
#include "mongo/db/extension/shared/extension_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo::extension::sdk {

/**
 * Base fixture for SDK unit tests.
 *
 * SDK adapters and handles read the negotiated capability version from the library-wide static in
 * negotiated_capability.h and route assertions through the HostServices handle, both of which
 * production code only ever sets from get_mongodb_extension(). A unit test never loads an
 * extension, so without intervention its SDK objects observe the zero "not yet negotiated"
 * capability and an unset HostServices: status conversions tassert, and inside a noexcept C API
 * trampoline that terminates the process. This fixture establishes both, so every test in the
 * binary is self-sufficient regardless of execution order or gtest filtering.
 *
 * Derived fixtures that need additional setup override setUp() and chain to SdkTest::setUp().
 */
class SdkTest : public unittest::Test {
public:
    static constexpr CapabilityVersion kTestCapability = CapabilityVersion::latestCapability();

    void setUp() override {
        // Set both through setHostServicesForTest: the version and the HostServices handle
        // describe the same negotiation, so they must never be established separately.
        HostServicesAPI::setHostServicesForTest(host_connector::getHostServices(kTestCapability),
                                                kTestCapability);
    }
};

}  // namespace mongo::extension::sdk
