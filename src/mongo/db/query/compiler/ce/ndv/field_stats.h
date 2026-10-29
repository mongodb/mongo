// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/uuid.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mongo::ce {

inline constexpr int kFieldStatsSchemaVersion = 1;

// Maximum number of fields a composite NDV statistic may cover. Enforced on user input by the
// analyze command and defensively by the $_internalConstructNdvSketch accumulator.
inline constexpr size_t kNdvMaxFields = 3;

/**
 * Builds the '_id' string for a field-stats document, e.g. "1|<collection uuid>|<field path>".
 * 'fieldPaths' must not be empty.
 *
 * A string key deliberately avoids BSON object equality, which is field-order sensitive and
 * therefore fragile as a lookup key. Shared by the write path (analyze command) and the read
 * path so both produce identical keys. The paths are sorted canonically, so the key is
 * independent of the order the caller lists them in, and '|' and '\' inside path names are
 * escaped so a path cannot fake a separator.
 */
std::string makeFieldStatsId(const UUID& collectionUuid, std::vector<std::string> fieldPaths);

}  // namespace mongo::ce
