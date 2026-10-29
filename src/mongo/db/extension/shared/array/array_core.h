// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once
#include "mongo/db/extension/public/api.h"
#include "mongo/util/modules.h"

#include <utility>  // std::declval

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
inline void destroyAbiArrayElem(ArrayElem_t& elt) noexcept;

template <typename T>
inline void destroyAbi(T*& node) noexcept {
    if (node && node->vtable && node->vtable->destroy) {
        node->vtable->destroy(node);
        node = nullptr;
    }
}

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
            destroyAbi(elt.parseOrAst.parse);
            break;
        }
        case kAstNode: {
            destroyAbi(elt.parseOrAst.ast);
            break;
        }
        default:
            // Memory is leaked if the type tag is invalid, but this only happens if the
            // extension violates the API contract.
            break;
    }
}

// MongoExtensionDPLArray specializations.
template <>
class AbiArrayType<::MongoExtensionDPLArrayElement> {
public:
    using type = ::MongoExtensionDPLArray;
};

template <>
inline void destroyAbiArrayElem(::MongoExtensionDPLArrayElement& elt) noexcept {
    switch (elt.type) {
        case kParse: {
            destroyAbi(elt.element.parseNode);
            break;
        }
        case kLogical: {
            destroyAbi(elt.element.logicalStage);
            break;
        }
        default:
            // Memory is leaked if the type tag is invalid, but this only happens if the
            // extension violates the API contract.
            break;
    }
}
}  // namespace mongo::extension
