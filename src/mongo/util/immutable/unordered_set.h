// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/util/immutable/details/memory_policy.h"
#include "mongo/util/modules.h"

#include <immer/set.hpp>
#include <immer/set_transient.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::immutable {


/**
 * Immutable unordered hash set.
 *
 * Interfaces that "modify" the hash set are 'const' and returns a new version of the set with
 * the modifications applied and leaves the original version untouched.
 *
 * It is optimized for efficient copies and low memory usage when multiple versions of the set
 * exist simultaneously at the expense of regular lookups not being as efficient as the regular
 * stdx unordered containers. Suitable for use in code that uses the copy-on-write
 * pattern.
 *
 * Thread-safety: All methods are const, it is safe to perform modifications that result in new
 * versions from multiple threads concurrently.
 *
 * Memory management: Internal memory management is done using reference counting, memory is free'd
 * as references to different versions of the set are released.
 *
 * Multiple modifications can be done efficiently using the 'transient()' interface.
 *
 * Documentation: 'immer/set.h' and https://sinusoid.es/immer/
 */
template <class T,
          class Hasher = DefaultHasher<T>,
          class Eq = absl::container_internal::hash_default_eq<T>>
using unordered_set = immer::set<T, EnsureTrustedHasher<Hasher, T>, Eq, detail::MemoryPolicy>;
}  // namespace mongo::immutable
