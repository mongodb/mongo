// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/immutable/details/memory_policy.h"

#include <immer/vector.hpp>
#include <immer/vector_transient.hpp>

namespace mongo::immutable {

/**
 * Immutable vector.
 *
 * Interfaces that "modify" the vector are 'const' and return a new version of the vector with
 * the modifications applied and leave the original version untouched.
 *
 * It is optimized for efficient copies and low memory usage when multiple versions of the vector
 * exist simultaneously at the expense of regular lookups not being as efficient as the regular
 * std::vector implementation. Suitable for use in code that uses the copy-on-write pattern.
 *
 * Thread-safety: All methods are const, it is safe to perform modifications that result in new
 * versions from multiple threads concurrently.
 *
 * Memory management: Internal memory management is done using reference counting, memory is free'd
 * as references to different versions of the vector are released.
 *
 * Multiple modifications can be done efficiently using the 'transient()' interface.
 *
 * Documentation: 'immer/vector.h' and https://sinusoid.es/immer/
 */
template <class T>
using vector = immer::vector<T, detail::MemoryPolicy>;
}  // namespace mongo::immutable
