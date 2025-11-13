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
#include "mongo/db/extension/public/api.h"

namespace mongo::extension {

// Trait maps ABI array element type to its RAII vector element type.
template <typename ArrayElem_t>
struct RaiiVectorElemType {
    // Default case
    using type = ArrayElem_t;
};

// Trait maps RAII vector element type to its ABI array element type.
template <typename VectorElem_t>
struct AbiArrayElemType {
    // Default case
    using type = VectorElem_t;
};

// Trait maps ABI array element type to its ABI array type.
// i.e ArrayElem_t -> Array_t.
template <typename ArrayElem_t>
struct AbiArrayType {
    // Default case
    using type = ArrayElem_t;
};

// Maps an Array_t to its underlying ArrayElem_t type.
template <typename Array_t>
struct UnderlyingArrayElemType {
    using ArrayElements_t = decltype(std::declval<Array_t>().elements);
    static_assert(std::is_pointer_v<ArrayElements_t>);
    using type = decltype(std::remove_pointer_t<ArrayElements_t>());
};

/**
 * destroyAbiArrayElem is a helper used by abiArrayToRaiiVector & raiiVecToAbiArray.
 * Each array element type must provide a specialization of this function, in order for either of
 * those templated utility functions to instantiate correctly.
 *
 * Each specialization must be noexcept, invoke the destructor of an ABI array element's
 * underlying pointer, and set the destroyed pointer's value to null. This function will generally
 * be used to destroy ABI array elements in the event that ownership could not be transferred to an
 * RAII handle (early exit).
 */
template <typename ArrayElem_t>
static inline void destroyAbiArrayElem(ArrayElem_t& elt) noexcept;

// MongoExtensionExpandedArray specializations.
template <>
class AbiArrayType<::MongoExtensionExpandedArrayElement> {
public:
    using type = ::MongoExtensionExpandedArray;
};

template <>
inline void destroyAbiArrayElem(::MongoExtensionExpandedArrayElement& elt) noexcept {
    switch (elt.type) {
        case kParseNode: {
            auto& parse = elt.parseOrAst.parse;
            if (parse && parse->vtable && parse->vtable->destroy) {
                parse->vtable->destroy(parse);
                parse = nullptr;
            }
            break;
        }
        case kAstNode: {
            auto& ast = elt.parseOrAst.ast;
            if (ast && ast->vtable && ast->vtable->destroy) {
                ast->vtable->destroy(ast);
                ast = nullptr;
            }
            break;
        }
        default:
            // Memory is leaked if the type tag is invalid, but this only happens if the
            // extension violates the API contract.
            break;
    }
}
}  // namespace mongo::extension
