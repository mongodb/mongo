// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/negotiated_capability.h"

#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/host_services.h"

namespace mongo::extension::sdk {
namespace {

/**
 * Holds the zero capability until get_mongodb_extension() runs. Zero doubles as the "not yet
 * negotiated" marker: it sits below legacyCapability(), and negotiation can never settle there,
 * so it cannot be confused with a version the host actually agreed to. It is the same sentinel
 * VTableAPI leaves behind in a moved-from handle.
 *
 * constinit because the REGISTER_EXTENSION registrars run during static initialization, ahead of
 * negotiation; constant-initializing this removes any ordering question about what they observe.
 */
constinit CapabilityVersion gNegotiatedCapabilityVersion = CapabilityVersion::zeroCapability();

}  // namespace

void setNegotiatedCapabilityVersion(const CapabilityVersion& capabilityVersion) {
    // The first real version wins.
    if (gNegotiatedCapabilityVersion == CapabilityVersion::zeroCapability()) {
        gNegotiatedCapabilityVersion = capabilityVersion;
    }
}

const CapabilityVersion& negotiatedCapabilityVersion() {
    return gNegotiatedCapabilityVersion;
}

void setNegotiatedCapabilityVersionForTest(const CapabilityVersion& capabilityVersion) {
    gNegotiatedCapabilityVersion = capabilityVersion;
}

}  // namespace mongo::extension::sdk
