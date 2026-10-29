// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#ifdef __has_include
#if __has_include("mongo/util/modules.h")
#include "mongo/util/modules.h"
#else
#define[[MONGO_MOD_PUBLIC]]
#endif
#endif  // __has_include

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * This header describes the Extensions API contract that the Host and all extensions must abide by.
 * This header is intentionally frozen and contains only what the two sides must agree on before
 * any API version is negotiated. All structs and APIs provided in this header are guaranteed to
 * remain stable across all major versions of the API.
 *
 * Extension loading is a two-step process driven by the host across two symbols that the
 * extension shared library must export.
 *
 *   1. get_mongodb_extension_versions: The host asks the extension to specify all the versions of
 *      the Extensions API the extension implements.
 *
 *   2. get_mongodb_extension: The host deliberates on whether any of those published versions are
 *      compatible with its own supported set. If a compatible version is found, the host explicitly
 *      requests that version by invoking this function with the chosen version. During this step,
 *      the host provides the HostServices corresponding to the requested API version.
 *
 * NOTE: get_mongodb_extension_versions is the only entry point fixed by this header, and it is
 * fixed only as a symbol name (GET_MONGODB_EXTENSION_VERSIONS_SYMBOL) and a function-pointer
 * typedef (get_mongodb_extension_versions_t). A future major version N of the API may result in
 * changes to the types used in get_mongodb_extension, which would require forking to a new
 * get_mongodb_extension_vN reflecting the new major version's changes.
 *
 * Maintenance rules:
 *   - This header must NEVER fork on an API major or otherwise reference a version-specific type.
 *     Version-varying surface - the Phase-2 instantiation entry point (get_mongodb_extension), the
 *     host-services and extension object types, and any type a future major might change - belongs
 *     in the per-version headers (api.h, api_v2.h, ...).
 *   - A future API major follows the symbol-naming convention: the per-major instantiation entry
 *     point is exported as get_mongodb_extension_vN, while the Phase-1 advertisement symbol in this
 *     header remains unchanged forever.
 *
 * NOTE: You must define both extension loading protocol symbols in your extension shared library
 * and avoid name mangling (for example, with 'extern "C"') so that the MongoDB server can find them
 * at loadtime.
 */

/**
 * Represents the API version of the MongoDB extension, to ensure compatibility between the MongoDB
 * server and the extension.
 *
 * The version is composed of two parts: major and minor. The major version is incremented
 * for incompatible changes and the minor version for backward-compatible changes.
 */
typedef struct {
    uint32_t major;
    uint32_t minor;
} MongoExtensionAPIVersion;

/**
 * A generic struct for a vector of Extensions API versions.
 *
 * Used for version compatibility checking. The data pointed to by 'versions' is read-only from
 * the receiver's perspective; the producer (host or extension, depending on direction) is
 * responsible for keeping the array alive for the lifetime of the struct's use.
 */
typedef struct {
    uint64_t len;
    const MongoExtensionAPIVersion* versions;
} MongoExtensionAPIVersionVector;

/**
 * Extension Loading - Phase 1 : the host asks the extension for the versions of the API it
 * implements. The host provides a MongoExtensionAPIVersionVector struct which the extension is
 * responsible for filling with its supported API versions. Any errors during the execution of this
 * function should result in the extensionVersions array being left empty.
 *
 * This call MUST NOT allow exceptions to escape across the C API boundary. No HostServices have
 * been delivered to the extension at this point, so host-routed assertion mechanisms are not
 * available and must not be used. Extension implementations must be noexcept in practice: if a
 * thrown exception escapes this function, behavior is undefined and the host has no way to
 * recover.
 */
#define GET_MONGODB_EXTENSION_VERSIONS_SYMBOL "get_mongodb_extension_versions"
typedef void (*get_mongodb_extension_versions_t)(MongoExtensionAPIVersionVector* extensionVersions);

#ifdef __cplusplus
}  // extern "C"
#endif
