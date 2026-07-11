// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

namespace mongo {

namespace timeseries {

/**
 * Scans all ready indexes on 'coll' and returns a map from user-facing field name to
 * 2dsphereIndexVersion for every 2dsphere_bucket index found. When multiple indexes cover the
 * same field, the version from the first index encountered is kept.
 */
[[MONGO_MOD_PUBLIC]] StringMap<int> build2dsphereIndexVersionMap(const Collection& coll);

}  // namespace timeseries
}  // namespace mongo
