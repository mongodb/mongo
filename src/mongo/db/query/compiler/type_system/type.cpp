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

}  // namespace

TypeSet::TypeSet(TypeMask types, TypeMask subsetTypes) : _types(types), _subsetTypes(subsetTypes) {
    // A type not in the set cannot be covered as a subset.
    dassert((_subsetTypes & ~_types) == 0);
}

TypeSet::TypeSet(BSONType type, Extent extent)
    : TypeSet(typeMask(type), isSubset(extent) ? typeMask(type) : 0) {}

bool TypeSet::hasType(BSONType type) const {
    return (_types & typeMask(type));
}

Extent TypeSet::getExtent(BSONType type) const {
    return (_subsetTypes & typeMask(type)) ? Extent::kSubset : Extent::kAll;
}

Type::Type(BSONType type, Extent extent) : Type(TypeSet(type, extent)) {}

Type::Type(TypeSet typeSet) : _typeSet(typeSet) {}

bool Type::hasType(BSONType type) const {
    return _typeSet.hasType(type);
}

Extent Type::getExtent(BSONType type) const {
    return _typeSet.getExtent(type);
}

}  // namespace mongo::pipeline::type_system
