// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/config.h"
#include "mongo/util/allocator_thread.h"

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#error This file should not be built
#endif

namespace mongo {

void startAllocatorThread() {
    // Do nothing
}

}  // namespace mongo
