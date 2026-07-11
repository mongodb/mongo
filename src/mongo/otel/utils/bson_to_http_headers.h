// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/stdx/unordered_map.h"

#include <string>
#include <vector>

namespace mongo::otel {
using HttpHeaderMap = stdx::unordered_map<std::string, std::vector<std::string>>;

/**
 * Tries to parse the provided BSON element into a map of HTTP header keys to arrays of values.
 * Each key holds an unordered vector of values, since each HTTP header may have multiple values
 * associated with it.
 */
[[nodiscard]] StatusWith<HttpHeaderMap> parseHttpHeadersFromBson(const BSONElement& headerDocument);
}  // namespace mongo::otel
