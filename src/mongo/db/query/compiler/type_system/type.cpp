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

/// Renders 'typeSet' as the negation of the types it does not cover, such as '~(null|array)'.
std::string renderNegation(const TypeSet& typeSet) {
    const std::string negatedUnion = renderUnion(complement(typeSet));
    if (negatedUnion.find('|') == std::string::npos) {
        return fmt::format("~{}", negatedUnion);
    }
    return fmt::format("~({})", negatedUnion);
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

/// Removes the object type from 'typeSet' and clears 'shape', which is what a 'never' field means.
void clearShape(TypeSet& typeSet, detail::Shape& shape) {
    typeSet = withoutType(typeSet, BSONType::object);
    shape = {};
}

/**
 * Returns pair [mergeInto, mergeFrom] set such that mergeInto is preferably uniquely owned.
 */
auto orderForMerge(detail::Shape lhs, detail::Shape rhs) {
    const bool preferRhs = lhs.fields.isShared() != rhs.fields.isShared()
        ? lhs.fields.isShared()
        : rhs.fields.size() > lhs.fields.size();
    if (preferRhs) {
        return std::make_pair(std::move(rhs), std::move(lhs));
    }
    return std::make_pair(std::move(lhs), std::move(rhs));
}

/// Returns true if 'lhs' and 'rhs' describe the same objects through the same storage.
bool sameShape(const detail::Shape& lhs, const detail::Shape& rhs) {
    return lhs.open == rhs.open && lhs.fields.sameStorage(rhs.fields);
}

/**
 * Returns the shape of the objects 'lhs' and 'rhs' describe when their fields are combined with
 * 'combine'. 'open' specifies the value for the resulting shape.
 * Removes the object type from 'typeSet' and clears the shape if any field becomes 'never'.
 */
template <typename Combine>
detail::Shape mergeShapes(
    TypeSet& typeSet, detail::Shape lhs, detail::Shape rhs, Open open, Combine combine) {
    // Fields combine the same way whichever shape they come from, so either may be merged into.
    auto [mergeInto, mergeFrom] = orderForMerge(std::move(lhs), std::move(rhs));
    bool hasNeverField = false;
    mergeInto.fields.merge(mergeFrom.fields,
                           impliedType(mergeInto.open),
                           impliedType(mergeFrom.open),
                           impliedType(open),
                           [&](Type lhsField, Type rhsField) {
                               Type merged = combine(std::move(lhsField), std::move(rhsField));
                               hasNeverField = hasNeverField || merged == Type::never();
                               return merged;
                           });
    mergeInto.open = open;
    if (hasNeverField) {
        clearShape(typeSet, mergeInto);
    }
    return mergeInto;
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

bool TypeSet::hasOnlyType(BSONType type) const {
    return _types == typeMask(type);
}

bool TypeSet::isNever() const {
    return _types == 0;
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
    const std::string negative = renderNegation(*this);

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

Type Type::anyObject() {
    return Type(BSONType::object, Extent::kAll);
}

Type Type::anyArray() {
    return Type(BSONType::array, Extent::kAll);
}

Type Type::someArray() {
    return Type(BSONType::array, Extent::kSubset);
}

Type Type::anyScalar() {
    return complement(unionType(Type::anyObject(), Type::anyArray()));
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

bool Type::hasOnlyType(BSONType type) const {
    return _typeSet.hasOnlyType(type);
}

bool Type::isNever() const {
    return _typeSet.isNever();
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

bool Type::canPathBeArray(const FieldRef& path) const {
    Type current = *this;
    for (size_t i = 0; i < path.numParts(); ++i) {
        if (current.hasType(BSONType::array)) {
            return true;
        }
        if (!current.hasType(BSONType::object)) {
            return false;
        }

        current = current.getField(path.getPart(i));
        if (current.hasType(BSONType::array)) {
            return true;
        }
    }
    return false;
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
    const std::string shape = renderShape(_shape);
    const std::string negative =
        fmt::format("{}|{}", renderNegation(withoutType(_typeSet, BSONType::object)), shape);
    const std::string positive = renderUnion(_typeSet, shape);
    return negative.size() < positive.size() ? negative : positive;
}

Type unionType(Type lhs, Type rhs) {
    auto typeSet = unionType(lhs._typeSet, rhs._typeSet);
    const bool lhsHasObject = lhs._typeSet.hasType(BSONType::object);
    const bool rhsHasObject = rhs._typeSet.hasType(BSONType::object);
    if (!lhsHasObject && !rhsHasObject) {
        return Type(typeSet);
    }
    // A side covering no object contributes no object to describe.
    if (!lhsHasObject) {
        return Type(typeSet, std::move(rhs._shape));
    }
    if (!rhsHasObject) {
        return Type(typeSet, std::move(lhs._shape));
    }
    if (sameShape(lhs._shape, rhs._shape)) {
        // Quick identity check.
        return Type(typeSet, std::move(lhs._shape));
    }
    if (isAnyObject(lhs._shape) || isAnyObject(rhs._shape)) {
        // A side covering every object leaves every object covered.
        return Type(typeSet);
    }
    // Union the individual fields in the shapes. Fields union the same way whichever shape they
    // come from, so either may be the one merged into.
    const Open open = lhs._shape.open || rhs._shape.open;
    auto merged = mergeShapes(typeSet,
                              std::move(lhs._shape),
                              std::move(rhs._shape),
                              open,
                              [](Type lhsField, Type rhsField) {
                                  return unionType(std::move(lhsField), std::move(rhsField));
                              });
    return Type(typeSet, std::move(merged));
}

Type intersectType(Type lhs, Type rhs) {
    auto typeSet = intersectType(lhs._typeSet, rhs._typeSet);
    if (!typeSet.hasType(BSONType::object)) {
        return Type(typeSet);
    }
    if (sameShape(lhs._shape, rhs._shape)) {
        // Quick identity check.
        return Type(typeSet, std::move(lhs._shape));
    }
    // A side covering every object narrows no field, which leaves the other side's shape.
    if (isAnyObject(lhs._shape)) {
        return Type(typeSet, std::move(rhs._shape));
    }
    if (isAnyObject(rhs._shape)) {
        return Type(typeSet, std::move(lhs._shape));
    }
    // Fields intersect the same way whichever shape they come from, so either may be the one
    // merged into.
    const Open open = lhs._shape.open && rhs._shape.open;
    auto merged = mergeShapes(typeSet,
                              std::move(lhs._shape),
                              std::move(rhs._shape),
                              open,
                              [](Type lhsField, Type rhsField) {
                                  return intersectType(std::move(lhsField), std::move(rhsField));
                              });
    return Type(typeSet, std::move(merged));
}

Type complement(Type type) {
    // We cannot represent the complement of a shape. If we have a shape, the object type is already
    // 'kSubset', so the complement will also have object(S).
    return Type(complement(type._typeSet));
}

Type narrowField(Type input, std::string_view fieldName, Type fieldType) {
    tassert(13459105,
            "Type should include object to be able to modify a field",
            input.hasType(BSONType::object));
    // Handing the field over to the intersection, rather than reading a copy of it, is what lets an
    // owned nested shape be narrowed without being copied.
    bool hasNeverField = false;
    input._shape.fields.update(fieldName, impliedType(input._shape.open), [&](Type current) {
        Type narrowed = intersectType(std::move(current), std::move(fieldType));
        hasNeverField = narrowed == Type::never();
        return narrowed;
    });
    if (hasNeverField) {
        clearShape(input._typeSet, input._shape);
    }
    return Type(input._typeSet, std::move(input._shape));
}

Type resolveFieldAccess(Type input, std::string_view fieldName) {
    tassert(13459501, "Cannot access a field of a type covering no value", !input.isNever());
    if (input.hasOnlyType(BSONType::object)) {
        return input.getField(fieldName);
    }
    // Any field access involving an array input results in 'any', because we currently don't model
    // array contents or array traversal semantics.
    if (input.hasType(BSONType::array)) {
        return Type::any();
    }
    // Field access on a scalar value results in 'missing', not 'never', which matches MQL
    // semantics. The input may also cover objects, whose field type must be kept.
    if (!input.hasType(BSONType::object)) {
        return Type::missing();
    }
    return unionType(input.getField(fieldName), Type::missing());
}
}  // namespace mongo::pipeline::type_system
