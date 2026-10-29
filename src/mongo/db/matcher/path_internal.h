// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/field_ref.h"
#include "mongo/util/modules.h"

#include <cstddef>

/**
 * TODO SERVER-114832 Break audit dependency on this class.
 */
namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {

/**
 * Finds the element at 'path' in 'doc', starting at 'startIndex' in 'path'. If none is found, an
 * EOO element is returned. If an array is encountered along 'path', the traversal stops early, and
 * the array is returned. 'idxPath' is set to the furthest index reached in 'path'.
 */
BSONElement getFieldDottedOrArray(const BSONObj& doc,
                                  const FieldRef& path,
                                  size_t* idxPath,
                                  size_t startIndex = 0);

}  // namespace mongo
