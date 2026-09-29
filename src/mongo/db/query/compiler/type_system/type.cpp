// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/type.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::pipeline::type_system {
namespace {

using detail::TypeMask;

/**
 * Bitmask of 'type', one bit per type.
 */
constexpr TypeMask typeMask(BSONType type) {
    switch (type) {
        case BSONType::minKey:
            return TypeMask{1} << 0;
        case BSONType::eoo:
            return TypeMask{1} << 1;
        case BSONType::numberDouble:
            return TypeMask{1} << 2;
        case BSONType::string:
            return TypeMask{1} << 3;
        case BSONType::object:
            return TypeMask{1} << 4;
        case BSONType::array:
            return TypeMask{1} << 5;
        case BSONType::binData:
            return TypeMask{1} << 6;
        case BSONType::undefined:
            return TypeMask{1} << 7;
        case BSONType::oid:
            return TypeMask{1} << 8;
        case BSONType::boolean:
            return TypeMask{1} << 9;
        case BSONType::date:
            return TypeMask{1} << 10;
        case BSONType::null:
            return TypeMask{1} << 11;
        case BSONType::regEx:
            return TypeMask{1} << 12;
        case BSONType::dbRef:
            return TypeMask{1} << 13;
        case BSONType::code:
            return TypeMask{1} << 14;
        case BSONType::symbol:
            return TypeMask{1} << 15;
        case BSONType::codeWScope:
            return TypeMask{1} << 16;
        case BSONType::numberInt:
            return TypeMask{1} << 17;
        case BSONType::timestamp:
            return TypeMask{1} << 18;
        case BSONType::numberLong:
            return TypeMask{1} << 19;
        case BSONType::numberDecimal:
            return TypeMask{1} << 20;
        case BSONType::maxKey:
            return TypeMask{1} << 21;
    }
    MONGO_UNREACHABLE;
}

template <typename It>
constexpr TypeMask typeMask(It first, It last) {
    TypeMask mask = 0;
    for (; first != last; ++first) {
        mask |= typeMask(*first);
    }
    return mask;
}

/// All BSONTypes.
constexpr std::array kAllTypes{
    BSONType::minKey,        BSONType::eoo,       BSONType::numberDouble, BSONType::string,
    BSONType::object,        BSONType::array,     BSONType::binData,      BSONType::undefined,
    BSONType::oid,           BSONType::boolean,   BSONType::date,         BSONType::null,
    BSONType::regEx,         BSONType::dbRef,     BSONType::code,         BSONType::symbol,
    BSONType::codeWScope,    BSONType::numberInt, BSONType::timestamp,    BSONType::numberLong,
    BSONType::numberDecimal, BSONType::maxKey};

/// A bitmask containing all BSONTypes.
constexpr TypeMask kAllTypesMask = typeMask(kAllTypes.begin(), kAllTypes.end());

/// Singleton types hold only one value.
constexpr std::array kSingletonTypes{
    BSONType::minKey, BSONType::eoo, BSONType::undefined, BSONType::null, BSONType::maxKey};

/// A bitmask containing all singleton types.
constexpr TypeMask kSingletonTypesMask = typeMask(kSingletonTypes.begin(), kSingletonTypes.end());

/// The types the 'number' alias stands for.
constexpr std::array kNumericTypes{
    BSONType::numberDouble, BSONType::numberInt, BSONType::numberLong, BSONType::numberDecimal};

/// A bitmask containing all types in the 'number' alias.
constexpr TypeMask kNumericTypesMask = typeMask(kNumericTypes.begin(), kNumericTypes.end());

}  // namespace

TypeSet::TypeSet(TypeMask types, TypeMask subsetTypes)
    : _types(types),
      // Singleton types are always Extent::kAll.
      _subsetTypes(subsetTypes & ~kSingletonTypesMask) {
    // A type not in the set cannot be covered as a subset.
    dassert((_subsetTypes & ~_types) == 0);
    // Other bits should not be used.
    dassert((_types & ~kAllTypesMask) == 0);
}

TypeSet::TypeSet(BSONType type, Extent extent)
    : TypeSet(typeMask(type), isSubset(extent) ? typeMask(type) : 0) {}

TypeSet TypeSet::any() {
    return TypeSet(kAllTypesMask, 0);
}

TypeSet TypeSet::never() {
    return TypeSet(0, 0);
}

TypeSet TypeSet::fromValue(const Value& value) {
    return TypeSet(value.getType(), Extent::kSubset);
}

TypeSet TypeSet::fromMatcherTypeSet(const MatcherTypeSet& matcherTypeSet) {
    // The 'number' alias is stored separately in MatcherTypeSet.
    TypeMask types = matcherTypeSet.allNumbers ? kNumericTypesMask : 0;
    types |= typeMask(matcherTypeSet.bsonTypes.begin(), matcherTypeSet.bsonTypes.end());
    return TypeSet(types, 0);
}

bool TypeSet::hasType(BSONType type) const {
    return (_types & typeMask(type));
}

Extent TypeSet::getExtent(BSONType type) const {
    return (_subsetTypes & typeMask(type)) ? Extent::kSubset : Extent::kAll;
}

TypeMask TypeSet::extentAllTypes() const {
    return _types & ~_subsetTypes;
}

TypeSet unionType(TypeSet lhs, TypeSet rhs) {
    TypeMask types = lhs._types | rhs._types;
    // A type covered in full on either side stays covered in full.
    TypeMask all = lhs.extentAllTypes() | rhs.extentAllTypes();
    // All other types as subsets.
    TypeMask subset = (lhs._subsetTypes | rhs._subsetTypes) & ~all;
    return TypeSet(types, subset);
}

TypeSet intersectType(TypeSet lhs, TypeSet rhs) {
    TypeMask types = lhs._types & rhs._types;
    // A type covered only in subset on either side remains subset,
    // and types not covered on either side are dropped.
    TypeMask subsets = (lhs._subsetTypes | rhs._subsetTypes) & types;
    return TypeSet(types, subsets);
}

TypeSet complement(TypeSet typeSet) {
    // The complement of a type covered as subset is also a subset of the same type.
    // Types covered in full are cleared, and types not covered become covered in full.
    TypeMask types = (typeSet._subsetTypes | ~typeSet._types) & kAllTypesMask;
    return TypeSet(types, typeSet._subsetTypes);
}

Type::Type(BSONType type, Extent extent) : Type(TypeSet(type, extent)) {}

Type::Type(TypeSet typeSet) : _typeSet(typeSet) {}

Type Type::any() {
    return Type(TypeSet::any());
}

Type Type::never() {
    return Type(TypeSet::never());
}

Type Type::fromValue(const Value& value) {
    return Type(TypeSet::fromValue(value));
}

Type Type::fromMatcherTypeSet(const MatcherTypeSet& matcherTypeSet) {
    return Type(TypeSet::fromMatcherTypeSet(matcherTypeSet));
}

TypeSet Type::getTypeSet() const {
    return _typeSet;
}

bool Type::hasType(BSONType type) const {
    return _typeSet.hasType(type);
}

Extent Type::getExtent(BSONType type) const {
    return _typeSet.getExtent(type);
}

Type unionType(Type lhs, Type rhs) {
    return Type(unionType(lhs.getTypeSet(), rhs.getTypeSet()));
}

Type intersectType(Type lhs, Type rhs) {
    return Type(intersectType(lhs.getTypeSet(), rhs.getTypeSet()));
}

Type complement(Type type) {
    return Type(complement(type.getTypeSet()));
}

}  // namespace mongo::pipeline::type_system
