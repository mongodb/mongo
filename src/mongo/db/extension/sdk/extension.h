// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/util/modules.h"

/**
 * This is the top-level header file for any MongoDB extension implementation. Each extension
 * must define two symbols, which together implement the two-phase load protocol:
 *
 *   - get_mongodb_extension_versions: publishes the set of API versions the extension supports.
 *     MUST NOT allow exceptions to escape across the C boundary; HostServices is not available
 *     during this call.
 *
 *   - get_mongodb_extension: instantiates the extension at the host-chosen version, receiving
 *     the matching HostServices pointer.
 *
 * These are the only symbols an extension shared library should export.
 */
extern "C" {
__attribute__((visibility("default"))) void get_mongodb_extension_versions(
    ::MongoExtensionAPIVersionVector* extensionVersions);

__attribute__((visibility("default"))) ::MongoExtensionStatus* get_mongodb_extension(
    ::MongoExtensionAPIVersion version,
    const ::MongoExtensionHostServices* hostServices,
    const ::MongoExtension** extension);
}
