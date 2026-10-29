// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/public/api.h"

// This extension intentionally defines only the get_mongodb_extension_versions and omits the
// get_mongodb_extension to simulate a malformed extension missing the part of the load protocol.

namespace {
static ::MongoExtensionAPIVersion kSupportedVersions[] = {MONGODB_EXTENSION_API_VERSION};
}  // namespace

extern "C" {
__attribute__((visibility("default"))) void get_mongodb_extension_versions(
    ::MongoExtensionAPIVersionVector* extensionVersions) noexcept {
    extensionVersions->len = sizeof(kSupportedVersions) / sizeof(kSupportedVersions[0]);
    extensionVersions->versions = kSupportedVersions;
}
// No definition of get_mongodb_extension — intentional.
}
