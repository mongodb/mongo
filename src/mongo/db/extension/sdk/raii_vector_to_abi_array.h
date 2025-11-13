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
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/shared/array/array_core.h"
#include "mongo/util/scopeguard.h"

namespace mongo::extension::sdk {

/**
 * RaiiAsArrayElem is a templated helper used by raiiVectorToAbiArray.
 * Specializations of this struct must implement:
 * 1) consume(): Method which consumes an RAII vector element (i.e Handle), releasing ownership of
 *               the element's underlying resource, and populating it into a target ABI array
 *               element.
 */
template <typename VectorElem_t>
struct RaiiAsArrayElem {
    using ArrayElem_t = AbiArrayElemType<VectorElem_t>::type;
    static void consume(ArrayElem_t& arrayElt, VectorElem_t&& vectorElt);
};

/**
 * This is a helper function responsible for turning a vector of RAII objects into an API boundary
 * array.
 *
 * When an extension transfers ownership of an RAII Vector (i.e Vector<OwnedHandle<>>) across the
 * API boundary, it must do so by populating an ABI output array with the vector's underlying
 * pointers. This function iterates over the vector, and transfers ownership of
 * the resources from the vector RAII elements into the output array.
 *
 * A scope guard ensures that in the event that we run into an error before the vector is
 * transferred into the array, the entries that have already been populated in the array will be
 * correctly cleaned up.
 *
 * Instantiations of this template must provide specializations for:
 *  - AbiArrayElemType
 *  - RaiiAsArrayElem
 *  - destroyAbiArrayElem
 */
template <typename VectorElem_t,
          typename Array_t = AbiArrayType<typename AbiArrayElemType<VectorElem_t>::type>::type>
void raiiVectorToAbiArray(std::vector<VectorElem_t> inputVector, Array_t& outputArray) {
    using RaiiAsArrayElem_t = RaiiAsArrayElem<VectorElem_t>;
    sdk_uassert(11113802,
                (str::stream() << "Input vector returned a different "
                                  "number of elements than expected: returned "
                               << inputVector.size() << ", but required " << outputArray.size),
                inputVector.size() == outputArray.size);

    // If we exit early, destroy the ABI nodes and null any raw pointers written to the
    // caller's buffer.
    size_t filled = 0;
    ScopeGuard guard([&]() noexcept {
        // Destroy elements already written to the output array.
        for (size_t i = 0; i < filled; ++i) {
            destroyAbiArrayElem(outputArray.elements[i]);
        }
        // Elements not yet written to the output array are still owned by the handle vector
        // and will be destroyed there instead.
    });

    // Populate the caller's buffer directly with raw pointers to nodes.
    for (size_t i = 0; i < inputVector.size(); ++i) {
        RaiiAsArrayElem_t::consume(outputArray.elements[i], std::move(inputVector[i]));
        ++filled;
    }
    guard.dismiss();
}
}  // namespace mongo::extension::sdk
