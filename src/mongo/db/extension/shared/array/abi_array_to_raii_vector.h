/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
