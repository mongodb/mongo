// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/extension/shared/array/array_core.h"
#include "mongo/util/modules.h"
#include "mongo/util/scopeguard.h"

#include <vector>

namespace mongo::extension {

/**
 * ArrayElemAsRaii is a templated helper used by abiArrayToRaiiVector.
 * Specializations of this struct must implement:
 * 1) consume(): Method which consumes an ABI array element, returning an RAII handle which assumes
 *               ownership of the array element's underlying resource. This function must also reset
 *               the state of the consumed array element (i.e set consumed pointer to nullptr).
 */
template <typename ArrayElem_t>
struct ArrayElemAsRaii {
    using VectorElem_t = RaiiVectorElemType<ArrayElem_t>::type;
    static VectorElem_t consume(ArrayElem_t& elt);
};

template <typename Array_t>
struct RaiiVector {
    using type = std::vector<
        typename RaiiVectorElemType<typename UnderlyingArrayElemType<Array_t>::type>::type>;
};

/**
 * This is a helper function responsible for turning an array of unowned raw pointers into a vector
 * of RAII Handles at the API boundary.
 *
 * When an array is populated by the extension, ownership is assumed to be passed to the caller.
 * This function iterates over the array, creates an RAII handle for each entry in the
 * array, and inserts it into a vector. The resulting vector is returned to the caller.
 *
 * Instantiations of this template must provide specializations for:
 *  - RaiiVectorElemType
 *  - ArrayElemAsRaii
 *  - destroyAbiArrayElem
 *
 * A scope guard ensures that in the event that we run into an error before the entire array is
 * transferred into RAII objects, the remaining elements in the array will be cleaned up correctly.
 */
template <typename Array_t>
auto abiArrayToRaiiVector(Array_t& inputArray) -> RaiiVector<Array_t>::type {
    using ArrayElem_t = UnderlyingArrayElemType<Array_t>::type;
    using Vector_t = RaiiVector<Array_t>::type;
    using ArrayElemAsRaii_t = ArrayElemAsRaii<ArrayElem_t>;
    const auto arrSize = inputArray.size;
    // This guard provides a best-effort cleanup in the case of an exception.
    //
    // - `transferredCount` tracks how many elements from the front of `buf` have been
    //   successfully wrapped into RAII handles and had their raw pointers nulled.
    // - If an exception occurs while constructing handles (e.g., OOM in `emplace_back` or a bad
    //   vtable), we destroy only the elements that have not yet been transferred
    //   ([transferredCount, arrSize)).
    size_t transferredCount{0};
    ScopeGuard guard([&]() {
        for (size_t idx = transferredCount; idx < arrSize; ++idx) {
            auto& elt = inputArray.elements[idx];
            destroyAbiArrayElem(elt);
        }
    });

    // Transfer ownership of each element into RAII handles and build the result vector.
    Vector_t outputVec;
    outputVec.reserve(arrSize);
    for (size_t idx = 0; idx < arrSize; ++idx) {
        auto& elt = inputArray.elements[idx];
        outputVec.emplace_back(ArrayElemAsRaii_t::consume(elt));
        ++transferredCount;
    }
    guard.dismiss();
    return outputVec;
}
}  // namespace mongo::extension
