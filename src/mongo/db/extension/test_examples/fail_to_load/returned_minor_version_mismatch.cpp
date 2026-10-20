// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/extension_status.h"

// This extension is implemented without the SDK-provided ExtensionAdapter in order to simulate an
// extension that violates the version-negotiation contract: it advertises a compatible API version
// during phase 1, but the MongoExtension returned from get_mongodb_extension reports a different
// minor version than the one the host negotiated. The SDK adapter always stamps the negotiated
// version, so a raw C API implementation is required. The host must reject this with 13693501
// before initialize() is ever called.
static ::MongoExtensionStatus* initialize_noop(const ::MongoExtension*,
                                               const ::MongoExtensionHostPortal*) noexcept {
    return nullptr;
}

static const ::MongoExtensionVTable vtable = {
    .initialize = &initialize_noop,
};

static const ::MongoExtension my_extension = {
    .vtable = &vtable,
    .version = ::MongoExtensionAPIVersion{MONGODB_EXTENSION_API_VERSION.major,
                                          MONGODB_EXTENSION_API_VERSION.minor + 1},
};

namespace {
static ::MongoExtensionAPIVersion kSupportedVersions[] = {MONGODB_EXTENSION_API_VERSION};
}  // namespace

extern "C" {
void get_mongodb_extension_versions(::MongoExtensionAPIVersionVector* extensionVersions) noexcept {
    extensionVersions->len = sizeof(kSupportedVersions) / sizeof(kSupportedVersions[0]);
    extensionVersions->versions = kSupportedVersions;
}

::MongoExtensionStatus* get_mongodb_extension(::MongoExtensionAPIVersion version,
                                              const ::MongoExtensionHostServices* hostServices,
                                              const ::MongoExtension** extension) {
    return mongo::extension::wrapCXXAndConvertExceptionToStatus(
        [&]() { *extension = &my_extension; });
}
}
