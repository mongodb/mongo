// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * Parses a hint. Returns the hint object, or if the element was a string the
 * string wrapped in an object of the form {"$hint": <index_name>}.
 */
BSONObj parseHint(const BSONElement& element);

/**
 * Writes the hint object if it is non-empty.
 */
void serializeHintToBSON(const BSONObj& hint, std::string_view fieldName, BSONObjBuilder* builder);

}  // namespace mongo
