// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace mongo {
/*
 * Validate wildcard index key pattern.
 */
[[MONGO_MOD_PUBLIC]] Status validateWildcardIndex(const BSONObj& keyPattern);

/*
 * Validate wildcardProjection field.
 */
[[MONGO_MOD_PUBLIC]] Status validateWildcardProjection(const BSONObj& keyPattern,
                                                       const BSONObj& pathProjection);
}  // namespace mongo
