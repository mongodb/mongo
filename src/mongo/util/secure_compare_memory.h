// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstddef>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Compare two arrays of bytes for equality in constant time.
 *
 * This means that the function runs for the same amount of time even if they differ. Unlike memcmp,
 * this function does not exit on the first difference.
 *
 * Returns true if the two arrays are equal.
 */
bool consttimeMemEqual(volatile const unsigned char* s1,  // NOLINT - using volatile to
                       volatile const unsigned char* s2,  // NOLINT - disable compiler optimizations
                       size_t length);

}  // namespace mongo
