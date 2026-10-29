// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>
#include <vector>

[[MONGO_MOD_PUBLIC]];

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
