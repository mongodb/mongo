// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/extension/public/api.h"
#include "mongo/util/modules.h"

#include <span>
namespace mongo::extension::sdk {
/**
 * Converts a MongoExtensionAPIVersionVector C struct to a std::span for easier usage.
 */
inline auto to_span(const ::MongoExtensionAPIVersionVector* vec) {
    return std::span<const ::MongoExtensionAPIVersion>(vec->versions,
                                                       static_cast<size_t>(vec->len));
}
}  // namespace mongo::extension::sdk
