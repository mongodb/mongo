// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/extension_status.h"

// This extension is implemented without the SDK-provided ExtensionAdapter in order to simulate a
// scenario in which the initialize function has a null pointer in the vtable. This is an unlikely
// scenario if an extension developer is building a C++ extension with our provided SDK, however, it
// is still a possible scenario that we should test and account for.
static const ::MongoExtensionVTable vtable = {
    .initialize = nullptr,
};

static const ::MongoExtension my_extension = {
    .vtable = &vtable,
    .version = MONGODB_EXTENSION_API_VERSION,
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
