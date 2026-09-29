// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/matcher_type_set.h"

namespace mongo::pipeline::type_system {

/**
 * Used to specify whether the BSON type covers the entire set of values of that type or a
 * portion of the values.
 */
enum class Extent : uint8_t {
    /// The entire set of values is covered.
    kAll,
    /// Only a subset of the set of values is covered.
    kSubset,
};

/// Returns true if 'extent' is Extent::kAll.
inline bool isAll(Extent extent) {
    return extent == Extent::kAll;
}

/// Returns true if 'extent' is Extent::kSubset.
inline bool isSubset(Extent extent) {
    return extent == Extent::kSubset;
}

namespace detail {
/// Integer type used to encode all BSONTypes.
using TypeMask = uint32_t;
}  // namespace detail

/**
 * Represents a set of BSON types.
 * Each type carries an Extent property.
 */
class TypeSet {
public:
    /// A TypeSet covering all or some of the values within 'type'.
    TypeSet(BSONType type, Extent extent);

    /// Returns true if 'type' is contained in the set.
    bool hasType(BSONType type) const;

    /// Returns the extent of 'type'. A type absent from the set is covered in full.
    Extent getExtent(BSONType type) const;

private:
    TypeSet(detail::TypeMask types, detail::TypeMask subsetTypes);

    // The types covered in this set.
    detail::TypeMask _types;
    /// The types covered only as a subset of their values.
    detail::TypeMask _subsetTypes;
};

/**
 * Represents a type in the type system.
 * A type can be a single BSONType, a union of BSONTypes and for objects, it could also have a
 * specific known set of fields.
 */
class Type {
public:
    /// A type covering all or some of the values within 'type'.
    Type(BSONType type, Extent extent);

    explicit Type(TypeSet typeSet);

    /// Returns true if 'type' is one of the types covered.
    bool hasType(BSONType type) const;

    /// Returns the extent of 'type'. A type which is not covered is covered in full.
    Extent getExtent(BSONType type) const;

private:
    TypeSet _typeSet;
};

}  // namespace mongo::pipeline::type_system
