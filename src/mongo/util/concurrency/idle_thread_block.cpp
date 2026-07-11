// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/concurrency/idle_thread_block.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace for_debuggers {
// This needs external linkage to ensure that debuggers can use it.
thread_local const char* idleThreadLocation = nullptr;
}  // namespace for_debuggers
using for_debuggers::idleThreadLocation;

void IdleThreadBlock::beginIdleThreadBlock(const char* location) {
    invariant(!idleThreadLocation);
    idleThreadLocation = location;
}

void IdleThreadBlock::endIdleThreadBlock() {
    invariant(idleThreadLocation);
    idleThreadLocation = nullptr;
}
}  // namespace mongo
