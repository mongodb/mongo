// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/query/compiler/type_system/field_map.h"

#include <string>
#include <string_view>

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
 * Specifies whether unnamed fields may hold any value.
 */
enum class Open : uint8_t {
    /// A field left unnamed may hold any value.
    kYes,
    /// A field left unnamed is missing.
    kNo,
};

/// Returns true if 'open' is Open::kYes.
inline bool isOpen(Open open) {
    return open == Open::kYes;
}

/// Returns kYes if a field left unnamed may hold any value in either of 'lhs' and 'rhs'.
inline Open operator||(Open lhs, Open rhs) {
    return isOpen(lhs) || isOpen(rhs) ? Open::kYes : Open::kNo;
}

/// Returns kYes if a field left unnamed may hold any value in both 'lhs' and 'rhs'.
inline Open operator&&(Open lhs, Open rhs) {
    return isOpen(lhs) && isOpen(rhs) ? Open::kYes : Open::kNo;
}

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

    /// A TypeSet covering all numeric types with the given extent.
    static TypeSet numericTypes(Extent extent);

    /// A TypeSet covering all or some of the values within 'type'.
    TypeSet(BSONType type, Extent extent);

    /// Returns true if 'type' is contained in the set.
    bool hasType(BSONType type) const;

    /// Returns true if 'type' is the only type contained in the set, whatever its extent.
    bool hasOnlyType(BSONType type) const;

    /// Returns true if the set covers no values at all.
    bool isNever() const;

    /// Returns the extent of 'type'. A type absent from the set is covered in full.
    Extent getExtent(BSONType type) const;

    /// Returns true if 'other' covers the same types with the same extents.
    bool operator==(const TypeSet& other) const = default;

    /**
     * Renders the set in the debug syntax, such as 'any', 'never', 'number|string' or '~array'.
     * A type covered as a subset carries a '(S)' suffix. Every set has exactly one rendering.
     */
    std::string toDebugString() const;

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

class Type;

namespace detail {

/**
 * Describes the objects a Type covers by naming the type of some of their fields.
 * A shape has exactly one encoding, so shapes describing the same objects compare equal.
 * Naming no field and leaving unnamed fields open describes every object.
 */
struct Shape {
    /// Returns true if 'other' describes the same objects.
    bool operator==(const Shape& other) const = default;

    /// Never holds a field of the implied type.
    FieldMap<Type> fields;
    /// Whether a field left unnamed may hold any value.
    Open open = Open::kYes;
};

}  // namespace detail

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

    /// A type covering BSONType::eoo (missing).
    static Type missing();

    /// A type covering BSONType::object.
    static Type anyObject();

    /// A type covering BSONType::array.
    static Type anyArray();

    /// A type covering a subset of BSONType::array.
    static Type someArray();

    /// A type covering any value except BSONType::object or BSONType::array
    static Type anyScalar();

    /// A type covering the type of 'value'. Generally a kSubset, except for singleton types.
    static Type fromValue(const Value& value);

    /// A type covering every value of every type named by 'matcherTypeSet'.
    static Type fromMatcherTypeSet(const MatcherTypeSet& matcherTypeSet);

    /**
     * A type covering the objects whose named fields hold the given types.
     * A field left unnamed holds any value if 'open' is kYes, and is missing otherwise.
     * Always Extent::kSubset, to prevent accidentally setting kAll when the fields map is empty.
     */
    static Type object(const StringMap<Type>& fields, Open open);

    /// A type covering all or some of the values within 'type'.
    Type(BSONType type, Extent extent);

    explicit Type(TypeSet typeSet);

    /// Returns the set of BSON types this type covers.
    TypeSet getTypeSet() const;

    /// Returns the shape of the objects covered.
    const detail::Shape& getShape_forTest() const;

    /// Returns true if 'type' is one of the types covered.
    bool hasType(BSONType type) const;

    /// Returns true if 'type' is the only type contained in the set, whatever its extent.
    bool hasOnlyType(BSONType type) const;

    /// Returns true if no value at all is covered.
    bool isNever() const;

    /// Returns the extent of 'type'. A type which is not covered is covered in full.
    Extent getExtent(BSONType type) const;

    /**
     * Returns the type of the 'fieldName' field of the covered object subset.
     * Returns 'never' if this type covers no object.
     */
    Type getField(std::string_view fieldName) const;

    /// Returns whether any component of 'path' may have BSON type array.
    bool canPathBeArray(const FieldRef& path) const;

    /**
     * Sets the type of the 'fieldName' field of the objects covered to 'fieldType'.
     * Does nothing if this type covers no object.
     */
    void setField(std::string_view fieldName, Type fieldType);

    /// Returns true if 'other' covers the same values with the same extent and fields.
    bool operator==(const Type& other) const;

    /**
     * Renders the type in the debug syntax, such as 'any', 'number|string', '~array',
     * '{x: number, ...}' or '~object|{x: number, ...}'.
     */
    std::string toDebugString() const;

private:
    friend Type unionType(Type lhs, Type rhs);
    friend Type intersectType(Type lhs, Type rhs);
    friend Type complement(Type type);
    friend Type narrowField(Type input, std::string_view fieldName, Type fieldType);

    /// Constructs a Type where the 'object' type is refined by the 'shape'.
    Type(TypeSet typeSet, detail::Shape shape);

    TypeSet _typeSet;
    /**
     * The shape of the objects covered. A shape describing every object leaves the 'object' type
     * covered in full, which is the only state the two may agree on.
     */
    detail::Shape _shape;
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

/**
 * Returns the type covering the values overlapped by both 'lhs' and 'rhs'.
 * The intersection of two incompatible object types removes the 'object' type.
 */
Type intersectType(Type lhs, Type rhs);

/**
 * Returns the type covering the values 'type' does not hold. A type covered as a subset remains
 * covered as subset.
 */
Type complement(Type type);

/**
 * Returns 'input' with the type of the 'fieldName' field of the object intersected with
 * 'fieldType', so the field never widens beyond it.
 * Returns 'input' unchanged if it covers no object.
 */
Type narrowField(Type input, std::string_view fieldName, Type fieldType);

/**
 * Returns the type describing the value 'fieldName' resolves to in a value of the input type.
 */
Type resolveFieldAccess(Type input, std::string_view fieldName);

}  // namespace mongo::pipeline::type_system
