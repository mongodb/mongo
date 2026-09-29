// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
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
    /// A TypeSet covering every value of every type, missing included.
    static TypeSet any();

    /// A TypeSet covering no values at all.
    static TypeSet never();

    /// A TypeSet covering the type of 'value'. Generally a kSubset, except for singleton types.
    static TypeSet fromValue(const Value& value);

    /// A TypeSet covering every value of every type named by 'matcherTypeSet'.
    static TypeSet fromMatcherTypeSet(const MatcherTypeSet& matcherTypeSet);

    /// A TypeSet covering all or some of the values within 'type'.
    TypeSet(BSONType type, Extent extent);

    /// Returns true if 'type' is contained in the set.
    bool hasType(BSONType type) const;

    /// Returns the extent of 'type'. A type absent from the set is covered in full.
    Extent getExtent(BSONType type) const;

    /// Returns true if 'other' covers the same types with the same extents.
    bool operator==(const TypeSet& other) const = default;

private:
    friend TypeSet unionType(TypeSet lhs, TypeSet rhs);
    friend TypeSet intersectType(TypeSet lhs, TypeSet rhs);
    friend TypeSet complement(TypeSet typeSet);

    TypeSet(detail::TypeMask types, detail::TypeMask subsetTypes);

    /// Bitset of the types whose every value is covered.
    detail::TypeMask extentAllTypes() const;

    /// The types covered in this set.
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
    /// A type covering every value of every type, missing included.
    static Type any();

    /// A type covering no values at all.
    static Type never();

    /// A type covering the type of 'value'. Generally a kSubset, except for singleton types.
    static Type fromValue(const Value& value);

    /// A type covering every value of every type named by 'matcherTypeSet'.
    static Type fromMatcherTypeSet(const MatcherTypeSet& matcherTypeSet);

    /// A type covering all or some of the values within 'type'.
    Type(BSONType type, Extent extent);

    explicit Type(TypeSet typeSet);

    /// Returns the set of BSON types this type covers.
    TypeSet getTypeSet() const;

    /// Returns true if 'type' is one of the types covered.
    bool hasType(BSONType type) const;

    /// Returns the extent of 'type'. A type which is not covered is covered in full.
    Extent getExtent(BSONType type) const;

    /// Returns true if 'other' covers the same types with the same extents.
    bool operator==(const Type& other) const = default;

private:
    TypeSet _typeSet;
};

/// Returns the values covered by 'lhs' or by 'rhs'.
TypeSet unionType(TypeSet lhs, TypeSet rhs);

/// Returns the values covered by both 'lhs' and 'rhs'.
TypeSet intersectType(TypeSet lhs, TypeSet rhs);

/**
 * Returns the values 'typeSet' does not cover. A type covered as a subset remains covered as
 * subset.
 */
TypeSet complement(TypeSet typeSet);

/// Returns the type covering the values of 'lhs' and of 'rhs'.
Type unionType(Type lhs, Type rhs);

/// Returns the type covering the values overlapped by both 'lhs' and 'rhs'.
Type intersectType(Type lhs, Type rhs);

/**
 * Returns the type covering the values 'type' does not hold. A type covered as a subset remains
 * covered as subset.
 */
Type complement(Type type);

}  // namespace mongo::pipeline::type_system
