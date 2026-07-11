// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

namespace fts {

extern const std::string WILDCARD;
extern const std::string INDEX_NAME;

enum TextIndexVersion {
    TEXT_INDEX_VERSION_INVALID = 0,  // Invalid value.
    TEXT_INDEX_VERSION_1 = 1,        // Legacy index format.  Deprecated.
    TEXT_INDEX_VERSION_2 = 2,        // Index format with ASCII support and murmur hashing.
    TEXT_INDEX_VERSION_3 = 3,        // Current index format with basic Unicode support.
};
}  // namespace fts
}  // namespace mongo
