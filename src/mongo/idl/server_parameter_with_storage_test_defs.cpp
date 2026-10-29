// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/atomic.h"

namespace mongo::test {
Atomic<int> gStdIntPreallocated;
Atomic<int> gStdIntPreallocatedUpdateCount;
size_t count;

}  // namespace mongo::test
