// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

namespace mongo::catalog_stats {

[[MONGO_MOD_PRIVATE]]
extern Atomic<int> requiresTimeseriesExtendedRangeSupport;

}  // namespace mongo::catalog_stats
