// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace count_request {
/**
 * Parses a limit for a CountCommandRequest. If the limit is negative, returns the absolute value.
 * Throws on invalid values.
 */
long long countParseLimit(const BSONElement& element);
}  // namespace count_request
}  // namespace mongo
