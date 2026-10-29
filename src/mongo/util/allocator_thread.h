// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

/**
 * Google TCMalloc has a background thread that needs to be started by the application to adjust
 * slab and size classes based on usage.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void startAllocatorThread();

}  // namespace mongo
