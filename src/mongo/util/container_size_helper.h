/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <vector>

namespace mongo {
namespace container_size_helper {
/**
 * Returns the estimate of the number of bytes consumed by the vector, based on the current capacity
 * of the vector and the size of objects of type T. Does not incorporate the size of any owned
 * objects that are pointed to by T. Also, does not incorporate sizeof(vector).
 */
template <class T>
uint64_t estimateObjectSizeInBytes(const std::vector<T>& vector) {
    return static_cast<uint64_t>(vector.capacity()) * sizeof(T);
}

/**
 * Returns the estimate of the number of bytes consumed by the container, based on the current
 * capacity of the container and the size of objects of type 'T::value_type'. Does not incorporate
 * the size of any owned objects that are pointed to by 'T::value_type'. Also, does not incorporate
 * sizeof(container).
 */
template <class T>
uint64_t estimateObjectSizeInBytes(const T& container) {
    return static_cast<uint64_t>(container.size()) * sizeof(typename T::value_type);
}

/**
 * Returns the estimate by recursively calculating the memory owned by each element of the
 * container. The 'function' should calculate the overall size of each individual element of the
 * 'container'.
 * When 'includeShallowSize' is true, adds the size of each container element.
 */
template <class T, class Function>
uint64_t estimateObjectSizeInBytes(const T& container, Function function, bool includeShallowSize) {
    uint64_t result = 0;
    for (const auto& element : container) {
        result += function(element);
    }
    result += includeShallowSize ? estimateObjectSizeInBytes(container) : 0;
    return result;
}
}  // namespace container_size_helper
}  // namespace mongo
