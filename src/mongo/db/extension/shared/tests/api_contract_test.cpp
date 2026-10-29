// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Freeze test for public/api_contract.h, the Extensions API loading contract that must remain
 * stable across all major versions of the API.
 *
 * The `frozen` namespace below contains twin declarations of every type, constant, and symbol in
 * api_contract.h, and the static_asserts compare the real declarations against their twins. All
 * comparisons are relative, so they pin the layout with no platform-specific constants.
 *
 * Any change to api_contract.h fails this build until the twins are updated in the same commit,
 * making every modification of the contract a deliberate, review-visible act.
 */

#include "mongo/db/extension/public/api_contract.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace mongo::extension {
namespace {

namespace frozen {

// Twin declarations of the frozen contract. DO NOT modify without a corresponding, deliberate
// change to public/api_contract.h in the same commit.
struct MongoExtensionAPIVersion {
    uint32_t major;
    uint32_t minor;
};

struct MongoExtensionAPIVersionVector {
    uint64_t len;
    const MongoExtensionAPIVersion* versions;
};

}  // namespace frozen

// MongoExtensionAPIVersion: layout and member types match the frozen twin.
static_assert(sizeof(MongoExtensionAPIVersion) == sizeof(frozen::MongoExtensionAPIVersion));
static_assert(alignof(MongoExtensionAPIVersion) == alignof(frozen::MongoExtensionAPIVersion));
static_assert(offsetof(MongoExtensionAPIVersion, major) ==
              offsetof(frozen::MongoExtensionAPIVersion, major));
static_assert(offsetof(MongoExtensionAPIVersion, minor) ==
              offsetof(frozen::MongoExtensionAPIVersion, minor));
static_assert(std::is_same_v<decltype(MongoExtensionAPIVersion::major),
                             decltype(frozen::MongoExtensionAPIVersion::major)>);
static_assert(std::is_same_v<decltype(MongoExtensionAPIVersion::minor),
                             decltype(frozen::MongoExtensionAPIVersion::minor)>);

// MongoExtensionAPIVersionVector: layout and member types match the frozen twin.
static_assert(sizeof(MongoExtensionAPIVersionVector) ==
              sizeof(frozen::MongoExtensionAPIVersionVector));
static_assert(alignof(MongoExtensionAPIVersionVector) ==
              alignof(frozen::MongoExtensionAPIVersionVector));
static_assert(offsetof(MongoExtensionAPIVersionVector, len) ==
              offsetof(frozen::MongoExtensionAPIVersionVector, len));
static_assert(offsetof(MongoExtensionAPIVersionVector, versions) ==
              offsetof(frozen::MongoExtensionAPIVersionVector, versions));
static_assert(std::is_same_v<decltype(MongoExtensionAPIVersionVector::len),
                             decltype(frozen::MongoExtensionAPIVersionVector::len)>);
// 'versions' is asserted literally rather than against the twin: its pointee is the real
// MongoExtensionAPIVersion, so a twin-relative is_same could never hold.
static_assert(std::is_same_v<decltype(MongoExtensionAPIVersionVector::versions),
                             const MongoExtensionAPIVersion*>);

// Phase-1 entry point: symbol name and function-pointer signature.
static_assert(std::string_view(GET_MONGODB_EXTENSION_VERSIONS_SYMBOL) ==
              "get_mongodb_extension_versions");
static_assert(
    std::is_same_v<get_mongodb_extension_versions_t, void (*)(MongoExtensionAPIVersionVector*)>);

}  // namespace
}  // namespace mongo::extension
