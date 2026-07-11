// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <cstddef>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Wrapper around std::malloc().
 * If std::malloc() fails, reports error with stack trace and exit.
 */
[[nodiscard]] MONGO_COMPILER_RETURNS_NONNULL MONGO_COMPILER_MALLOC
    MONGO_COMPILER_ALLOC_SIZE(1) void* mongoMalloc(size_t size);

/**
 * Wrapper around std::realloc().
 * If std::realloc() fails, reports error with stack trace and exit.
 */
[[nodiscard]] MONGO_COMPILER_RETURNS_NONNULL MONGO_COMPILER_ALLOC_SIZE(2) void* mongoRealloc(
    void* ptr, size_t size);

}  // namespace mongo
