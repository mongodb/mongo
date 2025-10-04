/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/stdx/trusted_hasher.h"
#include "mongo/util/immutable/details/memory_policy.h"

#include <immer/set.hpp>
#include <immer/set_transient.hpp>

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
