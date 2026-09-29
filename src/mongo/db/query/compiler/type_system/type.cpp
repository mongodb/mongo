// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/type.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/str_escape.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

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

/// Every type in the order for printing, which is based on BSON sort order.
constexpr std::array kTypesInPrintOrder{
    BSONType::minKey,       BSONType::eoo,       BSONType::undefined,  BSONType::null,
    BSONType::numberDouble, BSONType::numberInt, BSONType::numberLong, BSONType::numberDecimal,
    BSONType::string,       BSONType::symbol,    BSONType::object,     BSONType::array,
    BSONType::binData,      BSONType::oid,       BSONType::boolean,    BSONType::date,
    BSONType::timestamp,    BSONType::regEx,     BSONType::dbRef,      BSONType::code,
    BSONType::codeWScope,   BSONType::maxKey,
};

/**
 * Returns true if the numeric types are covered in full as the 'number' alias covers them,
 * which is when the alias can stand for all of them.
 */
bool canCollapseNumbers(const TypeSet& typeSet) {
    const TypeSet allNumbers = TypeSet::numericTypes(Extent::kAll);
    const TypeSet numbers = intersectType(typeSet, allNumbers);
    return numbers == allNumbers || numbers == TypeSet::numericTypes(Extent::kSubset);
}

/// Returns true if 'type' is one of the types the 'number' alias stands for.
bool isNumericType(BSONType type) {
    return typeMask(type) & kNumericTypesMask;
}

/// Returns 'typeSet' with 'type' removed.
TypeSet withoutType(TypeSet typeSet, BSONType type) {
    return intersectType(typeSet, complement(TypeSet(type, Extent::kAll)));
}

/**
 * Renders the types of 'typeSet' as a '|'-separated union in BSON sort order, suffixing the
 * ones covered only as a subset.
 * 'objectRendering' stands in for the object type.
 */
std::string renderUnion(const TypeSet& typeSet, const std::string& objectRendering = {}) {
    const bool collapseNumbers = canCollapseNumbers(typeSet);
    bool numberRendered = false;
    std::string rendered;
    for (auto type : kTypesInPrintOrder) {
        if (!typeSet.hasType(type)) {
            continue;
        }
        std::string_view name = typeName(type);
        if (isNumericType(type) && collapseNumbers) {
            // The 'number' alias stands for all four numeric types, so it is rendered once.
            if (numberRendered) {
                continue;
            }
            numberRendered = true;
            name = "number";
        }
        if (!rendered.empty()) {
            rendered += "|";
        }
        if (type == BSONType::object && !objectRendering.empty()) {
            rendered += objectRendering;
            continue;
        }
        rendered += name;
        if (isSubset(typeSet.getExtent(type))) {
            rendered += "(S)";
        }
    }
    return rendered;
}

/// Returns the type of unset fields (any or missing).
Type impliedType(Open open) {
    return isOpen(open) ? Type::any() : Type::missing();
}

/// Returns true if the object is not constrained by a shape.
bool isAnyObject(const detail::Shape& shape) {
    return shape.fields.empty() && isOpen(shape.open);
}

/// Returns the type of the field.
Type getFieldType(const detail::Shape& shape, std::string_view fieldName) {
    if (const Type* named = shape.fields.find(fieldName)) {
        return *named;
    }
    return impliedType(shape.open);
}

/// Stores 'fieldType' for 'fieldName', leaving a field of the implied type unnamed.
void storeField(detail::Shape& shape, std::string_view fieldName, Type fieldType) {
    if (fieldType == impliedType(shape.open)) {
        shape.fields.erase(fieldName);
        return;
    }
    shape.fields.set(fieldName, std::move(fieldType));
}

/// Escapes field names and adds quotes when needed.
std::string escapeFieldName(std::string_view fieldName) {
    if (std::all_of(fieldName.begin(), fieldName.end(), [](unsigned char c) {
            return std::isalnum(c) != 0;
        })) {
        return std::string{fieldName};
    }
    return fmt::format("\"{}\"", str::escapeForJSON(fieldName));
}

/// Renders the objects 'shape' describes, such as '{x: number}' or '{x: number, ...}'.
std::string renderShape(const detail::Shape& shape) {
    std::string rendered = "{";
    for (const auto& [fieldName, fieldType] : shape.fields) {
        if (rendered.size() > 1) {
            rendered += ", ";
        }
        rendered += escapeFieldName(fieldName);
        rendered += ": ";
        rendered += fieldType.toDebugString();
    }
    if (isOpen(shape.open)) {
        // An open shape naming no field is not stored, so a field always precedes this.
        rendered += ", ...";
    }
    rendered += "}";
    return rendered;
}

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

TypeSet TypeSet::numericTypes(Extent extent) {
    return TypeSet(kNumericTypesMask, isSubset(extent) ? kNumericTypesMask : 0);
}

bool TypeSet::hasType(BSONType type) const {
    return (_types & typeMask(type));
}

Extent TypeSet::getExtent(BSONType type) const {
    return (_subsetTypes & typeMask(type)) ? Extent::kSubset : Extent::kAll;
}

std::string TypeSet::toDebugString() const {
    if (*this == any()) {
        return "any";
    }
    if (*this == never()) {
        return "never";
    }

    // The complement of the union of the types that are not covered may be a shorter string.
    // This will happen generally when more types are included than excluded.
    // Since the four numeric types print as 'number' it is easier to produce this string and
    // compare the length than to try to count how many types will be rendered.
    const std::string negatedUnion = renderUnion(complement(*this));
    std::string negative;
    if (negatedUnion.find('|') == std::string::npos) {
        negative = fmt::format("~{}", negatedUnion);
    } else {
        negative = fmt::format("~({})", negatedUnion);
    }

    const std::string positive = renderUnion(*this);
    return negative.size() < positive.size() ? negative : positive;
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

// A default shape leaves every object covered, so there is no extent to narrow here.
Type::Type(TypeSet typeSet) : _typeSet(typeSet) {}

Type::Type(TypeSet typeSet, detail::Shape shape) : _typeSet(typeSet), _shape(std::move(shape)) {
    if (isAnyObject(_shape)) {
        // The shape carries no information in this case.
        return;
    }
    tassert(13459102,
            "Type should include object when a shape is specified",
            _typeSet.hasType(BSONType::object));
    // If we have a shape, we only represent a subset of all objects.
    _typeSet = intersectType(typeSet, complement(TypeSet(BSONType::object, Extent::kSubset)));
}

Type Type::any() {
    return Type(TypeSet::any());
}

Type Type::never() {
    return Type(TypeSet::never());
}

Type Type::missing() {
    return Type(BSONType::eoo, Extent::kAll);
}

Type Type::fromValue(const Value& value) {
    return Type(TypeSet::fromValue(value));
}

Type Type::fromMatcherTypeSet(const MatcherTypeSet& matcherTypeSet) {
    return Type(TypeSet::fromMatcherTypeSet(matcherTypeSet));
}

Type Type::object(const StringMap<Type>& fields, Open open) {
    detail::Shape shape;
    shape.open = open;
    for (const auto& [name, type] : fields) {
        if (type == Type::never()) {
            return type;
        }
        if (type != impliedType(open)) {
            shape.fields.set(name, type);
        }
    }
    return Type(TypeSet(BSONType::object, Extent::kSubset), std::move(shape));
}

TypeSet Type::getTypeSet() const {
    return _typeSet;
}

const detail::Shape& Type::getShape_forTest() const {
    return _shape;
}

bool Type::hasType(BSONType type) const {
    return _typeSet.hasType(type);
}

Extent Type::getExtent(BSONType type) const {
    return _typeSet.getExtent(type);
}

Type Type::getField(std::string_view fieldName) const {
    tassert(13459103,
            "Type should include object to be able to have a field",
            _typeSet.hasType(BSONType::object));
    return getFieldType(_shape, fieldName);
}

void Type::setField(std::string_view fieldName, Type fieldType) {
    tassert(13459104,
            "Type should include object to be able to set a field",
            _typeSet.hasType(BSONType::object));
    if (fieldType == never()) {
        // A field which can hold no value leaves no object to describe.
        *this = Type(withoutType(_typeSet, BSONType::object));
        return;
    }
    storeField(_shape, fieldName, std::move(fieldType));
    *this = Type(_typeSet, std::move(_shape));
}

bool Type::operator==(const Type& other) const {
    return _typeSet == other._typeSet && _shape == other._shape;
}

std::string Type::toDebugString() const {
    if (isAnyObject(_shape)) {
        return _typeSet.toDebugString();
    }
    return renderUnion(_typeSet, renderShape(_shape));
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
