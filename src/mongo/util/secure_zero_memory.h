// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstring>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Wrapper around several platform specific methods for zeroing memory
 * Memory zeroing is complicated by the fact that compilers will try to optimize it away, as the
 * memory frequently will not be later read.
 *
 * This function will, if available, perform a platform specific operation to zero memory. If no
 * platform specific operation is available, memory will be zeroed using volatile pointers.
 */
void secureZeroMemory(void* ptr, size_t size);


}  // namespace mongo
