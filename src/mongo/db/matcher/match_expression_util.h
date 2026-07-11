// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <cstddef>

namespace mongo::match_expression_util {
/**
 * Advances position of iterator 'iterator' by 'numberOfElements' elements but no more than the end
 * of the object.
 */
void advanceBy(size_t numberOfElements, BSONObjIterator& iterator);
}  // namespace mongo::match_expression_util
