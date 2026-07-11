// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/extension_status.h"

namespace {
static ::MongoExtensionAPIVersion kSupportedVersions[] = {MONGODB_EXTENSION_API_VERSION};
}  // namespace

extern "C" {
void get_mongodb_extension_versions(::MongoExtensionAPIVersionVector* extensionVersions) noexcept {
    extensionVersions->len = sizeof(kSupportedVersions) / sizeof(kSupportedVersions[0]);
    extensionVersions->versions = kSupportedVersions;
}

::MongoExtensionStatus* get_mongodb_extension(::MongoExtensionAPIVersion,
                                              const ::MongoExtensionHostServices*,
                                              const ::MongoExtension**) {
    // Intentionally leaves '*extension' as nullptr; the host should fire 10615503.
    return mongo::extension::wrapCXXAndConvertExceptionToStatus([&]() {});
}
}
