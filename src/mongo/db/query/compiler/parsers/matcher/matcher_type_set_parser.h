// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/util/modules.h"

namespace mongo::parsers::matcher {
/**
 * Given a mapping from string alias to BSON type, creates a MatcherTypeSet from a
 * BSONElement. This BSON alias may either represent a single type (via numerical type code or
 * string alias), or may be an array of types.
 *
 * Returns an error if the element cannot be parsed to a set of types.
 */
StatusWith<MatcherTypeSet> parseMatcherTypeSet(BSONElement);
}  // namespace mongo::parsers::matcher
