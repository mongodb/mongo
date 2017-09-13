/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <algorithm>

#include "mongo/stdx/new.h"

namespace mongo {

/**
 * Creates a new type with the same interface as T, but having the
 * given alignment. Note that if the given alignment is less than
 * alignof(T), the program is ill-formed. To ensure that your program
 * is well formed, see WithAligmentAtLeast, which will not reduce
 * alignment below the natural alignment of T.
 */
template <typename T, size_t alignment>
struct alignas(alignment) WithAlignment : T {
    using T::T;
};

/**
 * Creates a new type with the same interface as T, but having an
 * alignment greater than or equal to the given alignment. To ensure
 * that the program remains well formed, the alignment will not be
 * reduced below the natural alignment for T.
 */
template <typename T, size_t alignment>
using WithAlignmentAtLeast = WithAlignment<T, std::max(alignof(T), alignment)>;

/**
 * Creates a new type with the same interface as T but guaranteed to
 * be aligned to at least the size of a cache line.
 */
template <typename T>
using CacheAligned = WithAlignmentAtLeast<T, stdx::hardware_destructive_interference_size>;

}  // namespace mongo
