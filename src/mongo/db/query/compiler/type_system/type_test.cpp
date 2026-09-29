// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/type.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo::pipeline::type_system {

using namespace std::literals::string_view_literals;

namespace {

constexpr std::array kAllTypes{
    BSONType::minKey,        BSONType::eoo,       BSONType::numberDouble, BSONType::string,
    BSONType::object,        BSONType::array,     BSONType::binData,      BSONType::undefined,
    BSONType::oid,           BSONType::boolean,   BSONType::date,         BSONType::null,
    BSONType::regEx,         BSONType::dbRef,     BSONType::code,         BSONType::symbol,
    BSONType::codeWScope,    BSONType::numberInt, BSONType::timestamp,    BSONType::numberLong,
    BSONType::numberDecimal, BSONType::maxKey};

void assertCoversEveryType(const TypeSet& typeSet) {
    for (auto type : kAllTypes) {
        ASSERT_TRUE(typeSet.hasType(type)) << typeName(type);
        ASSERT_TRUE(isAll(typeSet.getExtent(type))) << typeName(type);
    }
}

void assertCoversNoType(const TypeSet& typeSet) {
    for (auto type : kAllTypes) {
        ASSERT_FALSE(typeSet.hasType(type)) << typeName(type);
        ASSERT_TRUE(isAll(typeSet.getExtent(type))) << typeName(type);
    }
}

void assertCoversEveryTypeExcept(const TypeSet& typeSet, BSONType excluded) {
    for (auto type : kAllTypes) {
        if (type == excluded) {
            ASSERT_FALSE(typeSet.hasType(type)) << typeName(type);
            continue;
        }
        ASSERT_TRUE(typeSet.hasType(type)) << typeName(type);
        ASSERT_TRUE(isAll(typeSet.getExtent(type))) << typeName(type);
    }
}

TypeSet unionOfEveryType() {
    auto typeSet = TypeSet::never();
    for (auto type : kAllTypes) {
        typeSet = unionType(typeSet, TypeSet(type, Extent::kAll));
    }
    return typeSet;
}

TEST(TypeSetTest, EveryTypeHasItsOwnBit) {
    for (auto type : kAllTypes) {
        TypeSet typeSet(type, Extent::kAll);
        for (auto other : kAllTypes) {
            if (other == type) {
                ASSERT_TRUE(typeSet.hasType(other));
                continue;
            }
            ASSERT_FALSE(typeSet.hasType(other));
        }
    }
}

TEST(TypeSetTest, FromBSONTypeCoveredInFull) {
    TypeSet typeSet(BSONType::numberInt, Extent::kAll);
    ASSERT_TRUE(typeSet.hasType(BSONType::numberInt));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberLong));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::numberInt)));
}

TEST(TypeSetTest, FromBSONTypeCoveredAsSubset) {
    TypeSet typeSet(BSONType::string, Extent::kSubset);
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::numberInt)));
}

TEST(TypeSetTest, FromBSONTypeHoldingSingleValueCoveredInFull) {
    for (auto type :
         {BSONType::minKey, BSONType::eoo, BSONType::undefined, BSONType::null, BSONType::maxKey}) {
        TypeSet typeSet(type, Extent::kSubset);
        ASSERT_TRUE(typeSet.hasType(type)) << typeName(type);
        ASSERT_TRUE(isAll(typeSet.getExtent(type))) << typeName(type);
    }
}

TEST(TypeSetTest, AnyCoversEveryTypeInFull) {
    assertCoversEveryType(TypeSet::any());
}

TEST(TypeSetTest, AnyEqualsUnionOfEveryType) {
    ASSERT_EQ(unionOfEveryType(), TypeSet::any());
}

TEST(TypeSetTest, NeverCoversNoType) {
    assertCoversNoType(TypeSet::never());
}

TEST(TypeSetTest, SetsCoveringTheSameValuesCompareEqual) {
    ASSERT_EQ(TypeSet(BSONType::string, Extent::kAll), TypeSet(BSONType::string, Extent::kAll));
}

TEST(TypeSetTest, FullAndSubsetSameTypeCompareUnequal) {
    ASSERT_NE(TypeSet(BSONType::string, Extent::kAll), TypeSet(BSONType::string, Extent::kSubset));
}

TEST(TypeSetTest, FromValueIsASubset) {
    auto typeSet = TypeSet::fromValue(Value("abc"sv));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, FromMissingValueCoversMissing) {
    auto typeSet = TypeSet::fromValue(Value());
    ASSERT_TRUE(typeSet.hasType(BSONType::eoo));
    ASSERT_FALSE(typeSet.hasType(BSONType::null));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::eoo)));
}

TEST(TypeSetTest, FromValueHoldingSingleValueCoversInFull) {
    for (auto value :
         {Value(), Value(BSONNULL), Value(BSONUndefined), Value(MINKEY), Value(MAXKEY)}) {
        auto typeSet = TypeSet::fromValue(value);
        ASSERT_TRUE(isAll(typeSet.getExtent(value.getType()))) << typeName(value.getType());
    }
}

TEST(TypeSetTest, ComplementOfTypeHoldingSingleValueExcludesIt) {
    auto typeSet = complement(TypeSet(BSONType::eoo, Extent::kSubset));
    ASSERT_FALSE(typeSet.hasType(BSONType::eoo));
    ASSERT_TRUE(typeSet.hasType(BSONType::null));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::null)));
}

TEST(TypeSetTest, FromMatcherTypeSetWithNamedTypes) {
    MatcherTypeSet matcherTypeSet;
    matcherTypeSet.bsonTypes = {BSONType::string, BSONType::date};
    auto typeSet = TypeSet::fromMatcherTypeSet(matcherTypeSet);
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(typeSet.hasType(BSONType::date));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, FromMatcherTypeSetWithNumberAlias) {
    MatcherTypeSet matcherTypeSet;
    matcherTypeSet.allNumbers = true;
    auto typeSet = TypeSet::fromMatcherTypeSet(matcherTypeSet);
    ASSERT_TRUE(typeSet.hasType(BSONType::numberDouble));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberLong));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberDecimal));
    ASSERT_FALSE(typeSet.hasType(BSONType::string));
}

TEST(TypeSetTest, FromEmptyMatcherTypeSetCoversNoType) {
    assertCoversNoType(TypeSet::fromMatcherTypeSet(MatcherTypeSet()));
}

TEST(TypeSetTest, UnionOfDistinctTypesKeepsEachExtent) {
    auto typeSet = unionType(TypeSet(BSONType::string, Extent::kSubset),
                             TypeSet(BSONType::numberInt, Extent::kAll));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::numberInt)));
}

TEST(TypeSetTest, UnionOfFullAndSubsetSameTypeIsFull) {
    auto typeSet = unionType(TypeSet(BSONType::string, Extent::kSubset),
                             TypeSet(BSONType::string, Extent::kAll));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, UnionOfSubsetSameTypeStaysSubset) {
    auto typeSet = unionType(TypeSet(BSONType::string, Extent::kSubset),
                             TypeSet(BSONType::string, Extent::kSubset));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, UnionOfFullSameTypeStaysFull) {
    auto typeSet =
        unionType(TypeSet(BSONType::string, Extent::kAll), TypeSet(BSONType::string, Extent::kAll));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, UnionWithNeverIsUnchanged) {
    auto typeSet = unionType(TypeSet(BSONType::string, Extent::kSubset), TypeSet::never());
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, UnionWithAnyIsAny) {
    for (auto typeSet : {TypeSet(BSONType::string, Extent::kAll),
                         TypeSet(BSONType::string, Extent::kSubset),
                         TypeSet::never(),
                         TypeSet::any()}) {
        ASSERT_EQ(TypeSet::any(), unionType(typeSet, TypeSet::any()));
    }
}

TEST(TypeSetTest, IntersectionOfFullAndSubsetSameTypeIsSubset) {
    auto typeSet = intersectType(TypeSet(BSONType::string, Extent::kAll),
                                 TypeSet(BSONType::string, Extent::kSubset));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, IntersectionOfSubsetSameTypeStaysSubset) {
    auto typeSet = intersectType(TypeSet(BSONType::string, Extent::kSubset),
                                 TypeSet(BSONType::string, Extent::kSubset));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, IntersectionOfFullSameTypeStaysFull) {
    auto typeSet = intersectType(TypeSet(BSONType::string, Extent::kAll),
                                 TypeSet(BSONType::string, Extent::kAll));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, IntersectionOfDistinctTypesCoversNoType) {
    assertCoversNoType(intersectType(TypeSet(BSONType::string, Extent::kSubset),
                                     TypeSet(BSONType::numberInt, Extent::kAll)));
}

TEST(TypeSetTest, IntersectionWithAnyIsUnchanged) {
    for (auto typeSet : {TypeSet(BSONType::string, Extent::kAll),
                         TypeSet(BSONType::string, Extent::kSubset),
                         TypeSet::never(),
                         TypeSet::any()}) {
        ASSERT_EQ(typeSet, intersectType(typeSet, TypeSet::any()));
    }
}

TEST(TypeSetTest, IntersectionWithNeverCoversNoType) {
    assertCoversNoType(intersectType(TypeSet(BSONType::string, Extent::kAll), TypeSet::never()));
}

TEST(TypeSetTest, ComplementOfAnyCoversNoType) {
    assertCoversNoType(complement(TypeSet::any()));
}

TEST(TypeSetTest, ComplementOfNeverCoversEveryType) {
    assertCoversEveryType(complement(TypeSet::never()));
}

TEST(TypeSetTest, ComplementOfUnionOfEveryTypeEqualsNever) {
    ASSERT_EQ(complement(unionOfEveryType()), TypeSet::never());
}

TEST(TypeSetTest, ComplementOfFullTypeExcludesOnlyThatType) {
    assertCoversEveryTypeExcept(complement(TypeSet(BSONType::string, Extent::kAll)),
                                BSONType::string);
}

TEST(TypeSetTest, ComplementOfSubsetTypeKeepsItASubset) {
    auto typeSet = complement(TypeSet(BSONType::string, Extent::kSubset));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
    ASSERT_TRUE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::numberInt)));
}

TEST(TypeSetTest, ComplementOfPartlySubsetUnionRemovesOnlyTheFullType) {
    auto typeSet = complement(unionType(TypeSet(BSONType::string, Extent::kSubset),
                                        TypeSet(BSONType::numberInt, Extent::kAll)));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
    ASSERT_TRUE(typeSet.hasType(BSONType::eoo));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::eoo)));
}

TEST(TypeSetTest, ComplementOfComplementRestoresSubsetType) {
    auto negated = complement(TypeSet(BSONType::string, Extent::kSubset));
    auto typeSet = complement(negated);
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(isSubset(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, ComplementOfComplementRestoresFullType) {
    auto negated = complement(TypeSet(BSONType::string, Extent::kAll));
    auto typeSet = complement(negated);
    ASSERT_TRUE(typeSet.hasType(BSONType::string));
    ASSERT_FALSE(typeSet.hasType(BSONType::numberInt));
    ASSERT_TRUE(isAll(typeSet.getExtent(BSONType::string)));
}

TEST(TypeSetTest, AnyRendersAsAny) {
    ASSERT_EQ(TypeSet::any().toDebugString(), "any");
}

TEST(TypeSetTest, NeverRendersAsNever) {
    ASSERT_EQ(TypeSet::never().toDebugString(), "never");
}

TEST(TypeSetTest, ComplementOfAnyRendersAsNever) {
    ASSERT_EQ(complement(TypeSet::any()).toDebugString(), "never");
}

TEST(TypeSetTest, FullTypeRendersItsName) {
    ASSERT_EQ(TypeSet(BSONType::string, Extent::kAll).toDebugString(), "string");
}

TEST(TypeSetTest, SubsetTypeIsSuffixed) {
    ASSERT_EQ(TypeSet(BSONType::string, Extent::kSubset).toDebugString(), "string(S)");
}

TEST(TypeSetTest, MissingTypeRendersAsMissing) {
    ASSERT_EQ(TypeSet(BSONType::eoo, Extent::kAll).toDebugString(), "missing");
}

TEST(TypeSetTest, UnionRendersInSortOrder) {
    auto typeSet = unionType(
        TypeSet(BSONType::object, Extent::kAll),
        unionType(TypeSet(BSONType::string, Extent::kAll), TypeSet(BSONType::null, Extent::kAll)));
    ASSERT_EQ(typeSet.toDebugString(), "null|string|object");
}

TEST(TypeSetTest, EveryNumericTypeCollapsesToNumber) {
    MatcherTypeSet matcherTypeSet;
    matcherTypeSet.allNumbers = true;
    ASSERT_EQ(TypeSet::fromMatcherTypeSet(matcherTypeSet).toDebugString(), "number");
}

TEST(TypeSetTest, SomeNumericTypesRenderIndividually) {
    auto typeSet = unionType(TypeSet(BSONType::numberInt, Extent::kAll),
                             TypeSet(BSONType::numberLong, Extent::kAll));
    ASSERT_EQ(typeSet.toDebugString(), "int|long");
}

TEST(TypeSetTest, NumericTypesDisagreeingOnExtentRenderIndividually) {
    auto typeSet = unionType(TypeSet(BSONType::numberDouble, Extent::kSubset),
                             unionType(TypeSet(BSONType::numberInt, Extent::kAll),
                                       unionType(TypeSet(BSONType::numberLong, Extent::kAll),
                                                 TypeSet(BSONType::numberDecimal, Extent::kAll))));
    ASSERT_EQ(typeSet.toDebugString(), "double(S)|int|long|decimal");
}

TEST(TypeSetTest, SubsetNumericTypeAmongFullOnesRendersIndividually) {
    auto typeSet = unionType(TypeSet(BSONType::numberInt, Extent::kSubset),
                             unionType(TypeSet(BSONType::numberDouble, Extent::kAll),
                                       unionType(TypeSet(BSONType::numberLong, Extent::kAll),
                                                 TypeSet(BSONType::numberDecimal, Extent::kAll))));
    ASSERT_EQ(typeSet.toDebugString(), "double|int(S)|long|decimal");
}

TEST(TypeSetTest, EveryNumericTypeAsSubsetCollapsesToSubsetNumber) {
    auto typeSet =
        unionType(TypeSet(BSONType::numberDouble, Extent::kSubset),
                  unionType(TypeSet(BSONType::numberInt, Extent::kSubset),
                            unionType(TypeSet(BSONType::numberLong, Extent::kSubset),
                                      TypeSet(BSONType::numberDecimal, Extent::kSubset))));
    ASSERT_EQ(typeSet.toDebugString(), "number(S)");
}

TEST(TypeSetTest, ComplementOfOneTypeRendersAsNegation) {
    auto typeSet = complement(TypeSet(BSONType::array, Extent::kAll));
    ASSERT_EQ(typeSet.toDebugString(), "~array");
}

TEST(TypeSetTest, ComplementOfUnionIsParenthesised) {
    auto typeSet = complement(
        unionType(TypeSet(BSONType::null, Extent::kAll), TypeSet(BSONType::array, Extent::kAll)));
    ASSERT_EQ(typeSet.toDebugString(), "~(null|array)");
}

TEST(TypeSetTest, ComplementOfSubsetTypeKeepsSuffix) {
    auto typeSet = complement(TypeSet(BSONType::string, Extent::kSubset));
    ASSERT_EQ(typeSet.toDebugString(), "~string(S)");
}

TEST(TypeSetTest, TypesSortingAtOrBelowNumbersRenderAsUnion) {
    MatcherTypeSet matcherTypeSet;
    matcherTypeSet.allNumbers = true;
    auto typeSet =
        unionType(TypeSet::fromMatcherTypeSet(matcherTypeSet),
                  unionType(TypeSet(BSONType::minKey, Extent::kAll),
                            unionType(TypeSet(BSONType::eoo, Extent::kAll),
                                      unionType(TypeSet(BSONType::undefined, Extent::kAll),
                                                TypeSet(BSONType::null, Extent::kAll)))));
    ASSERT_EQ(typeSet.toDebugString(), "minKey|missing|undefined|null|number");
}

TEST(TypeTest, RendersItsTypeSet) {
    ASSERT_EQ(Type(BSONType::string, Extent::kAll).toDebugString(), "string");
}

TEST(TypeTest, ComplementRendersAsNegation) {
    auto type = complement(Type(BSONType::array, Extent::kAll));
    ASSERT_EQ(type.toDebugString(), "~array");
}

Type object(Extent extent) {
    return Type(BSONType::object, extent);
}

Type openObject(const StringMap<Type>& fields) {
    return Type::object(std::move(fields), Open::kYes);
}

Type closedObject(const StringMap<Type>& fields) {
    return Type::object(std::move(fields), Open::kNo);
}

Type allValues(BSONType type) {
    return Type(type, Extent::kAll);
}

/// Identifies the field storage of the shape without holding a reference to it.
const void* getFieldStorage(const Type& type) {
    return type.getShape_forTest().fields.getStorage_forTest();
}

/// Returns true if the shape leaves every object covered, so it carries no information.
bool describesEveryObject(const Type& type) {
    const auto& shape = type.getShape_forTest();
    return type.hasType(BSONType::object) && shape.fields.empty() && isOpen(shape.open);
}

TEST(TypeTest, OpenObjectWithoutFieldsDescribesOnlySomeObjects) {
    auto type = Type::object({}, Open::kYes);
    // The object() factory always covers a subset, even if fields are empty.
    ASSERT_TRUE(isSubset(type.getExtent(BSONType::object)));
    ASSERT_TRUE(describesEveryObject(type));
    ASSERT_TRUE(isOpen(type.getShape_forTest().open));
}

TEST(TypeTest, ClosedObjectWithoutFieldsDescribesOnlySomeObjects) {
    auto type = Type::object({}, Open::kNo);
    ASSERT_TRUE(isSubset(type.getExtent(BSONType::object)));
    ASSERT_FALSE(describesEveryObject(type));
    ASSERT_FALSE(isOpen(type.getShape_forTest().open));
}

TEST(TypeTest, ObjectWithKnownFieldsIsASubset) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_TRUE(isSubset(type.getExtent(BSONType::object)));
}

TEST(TypeTest, ShapeIsDroppedWhenTheObjectTypeIsNotCovered) {
    auto type = intersectType(openObject({{"x", allValues(BSONType::string)}}),
                              allValues(BSONType::string));
    ASSERT_FALSE(type.hasType(BSONType::object));
}

TEST(TypeTest, OpenObjectDoesNotStoreAFieldOfTypeAny) {
    auto type = openObject({{"x", Type::any()}});
    ASSERT_TRUE(describesEveryObject(type));
}

TEST(TypeTest, ClosedObjectDoesNotStoreAMissingField) {
    auto type = closedObject({{"x", Type::missing()}});
    ASSERT_FALSE(describesEveryObject(type));
    ASSERT_TRUE(type.getShape_forTest().fields.empty());
}

TEST(TypeTest, ClosedObjectStoresAFieldOfTypeAny) {
    auto type = closedObject({{"x", Type::any()}});
    ASSERT_EQ(type.getShape_forTest().fields.size(), 1u);
}

TEST(TypeTest, OpenObjectStoresAMissingField) {
    auto type = openObject({{"x", Type::missing()}});
    ASSERT_EQ(type.getShape_forTest().fields.size(), 1u);
}

TEST(TypeTest, FieldCoveringNoValueRemovesTheObjectType) {
    auto type = openObject({{"x", Type::never()}});
    ASSERT_FALSE(type.hasType(BSONType::object));
    ASSERT_EQ(type, Type::never());
}

TEST(TypeTest, FieldCoveringNoValueLeavesTheOtherTypesCovered) {
    auto type = unionType(openObject({{"x", Type::never()}}), allValues(BSONType::string));
    ASSERT_EQ(type, allValues(BSONType::string));
}

TEST(TypeTest, NestedFieldCoveringNoValueRemovesTheOuterObjectType) {
    auto type = openObject({{"a", openObject({{"x", Type::never()}})}});
    ASSERT_EQ(type, Type::never());
}

TEST(TypeTest, ClosedObjectWithAllFieldsMissingNamesNoField) {
    auto type = closedObject({{"x", Type::missing()}, {"y", Type::missing()}});
    ASSERT_EQ(type, closedObject({}));
}

TEST(TypeTest, GetFieldReturnsTheTypeOfANamedField) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(type.getField("x"), allValues(BSONType::string));
}

TEST(TypeTest, GetFieldOnOpenObjectReturnsAnyForAnUnnamedField) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(type.getField("y"), Type::any());
}

TEST(TypeTest, GetFieldOnClosedObjectReturnsMissingForAnUnnamedField) {
    auto type = closedObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(type.getField("y"), Type::missing());
}

TEST(TypeTest, GetFieldOnBareObjectTypeReturnsAny) {
    ASSERT_EQ(object(Extent::kAll).getField("x"), Type::any());
}

TEST(TypeTest, GetFieldRequiresObjectType) {
    ASSERT_TASSERT_CODE(allValues(BSONType::string).getField("x"), 13459103);
}

TEST(TypeTest, SetFieldNarrowsTheNamedField) {
    auto type = object(Extent::kAll);
    type.setField("x", allValues(BSONType::string));
    ASSERT_EQ(type.getField("x"), allValues(BSONType::string));
    ASSERT_EQ(type.getField("y"), Type::any());
}

TEST(TypeTest, SetFieldToAnyOnOpenObjectDropsTheField) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    type.setField("x", Type::any());
    ASSERT_TRUE(describesEveryObject(type));
}

TEST(TypeTest, SetFieldToMissingOnClosedObjectDropsTheField) {
    auto type = closedObject({{"x", allValues(BSONType::string)}});
    type.setField("x", Type::missing());
    ASSERT_EQ(type, closedObject({}));
}

TEST(TypeTest, SetFieldToNeverRemovesTheObjectType) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    type.setField("y", Type::never());
    ASSERT_FALSE(type.hasType(BSONType::object));
    ASSERT_EQ(type, Type::never());
}

TEST(TypeTest, SetFieldOnUnsharedShapeReusesIt) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    const void* shape = getFieldStorage(type);
    type.setField("y", allValues(BSONType::numberInt));
    ASSERT_EQ(getFieldStorage(type), shape);
    ASSERT_EQ(type.getField("y"), allValues(BSONType::numberInt));
}

TEST(TypeTest, SetFieldOnSharedShapeClonesIt) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    auto shared = type;
    type.setField("x", allValues(BSONType::numberInt));
    ASSERT_NE(getFieldStorage(type), getFieldStorage(shared));
    ASSERT_EQ(shared.getField("x"), allValues(BSONType::string));
}

TEST(TypeTest, SetFieldToSameTypeKeepsShape) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    const void* shape = getFieldStorage(type);
    type.setField("x", allValues(BSONType::string));
    ASSERT_EQ(getFieldStorage(type), shape);
}

TEST(TypeTest, SetFieldOnCopyLeavesTheOriginalUnchanged) {
    auto original = openObject({{"x", allValues(BSONType::string)}});
    auto narrowed = original;
    narrowed.setField("x", allValues(BSONType::numberInt));
    ASSERT_EQ(original.getField("x"), allValues(BSONType::string));
    ASSERT_EQ(narrowed.getField("x"), allValues(BSONType::numberInt));
}

TEST(TypeTest, SetFieldOnCopyLeavesTheNestedFieldsOfTheOriginal) {
    auto original = openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}});
    auto narrowed = original;
    narrowed.setField("a", openObject({{"b", allValues(BSONType::numberInt)}}));
    ASSERT_EQ(original.getField("a").getField("b"), allValues(BSONType::string));
    ASSERT_EQ(narrowed.getField("a").getField("b"), allValues(BSONType::numberInt));
}

TEST(TypeTest, SetFieldOnCopyKeepsTheShapeOfAnUntouchedNestedField) {
    auto original = openObject({{"a", openObject({{"b", allValues(BSONType::string)}})},
                                {"x", allValues(BSONType::string)}});
    const void* nested = getFieldStorage(original.getField("a"));
    auto narrowed = original;
    narrowed.setField("x", allValues(BSONType::numberInt));
    ASSERT_NE(getFieldStorage(narrowed), getFieldStorage(original));
    ASSERT_EQ(getFieldStorage(narrowed.getField("a")), nested);
}

TEST(TypeTest, SuccessiveSetFieldCallsCopyTheSharedShapeOnce) {
    auto original = openObject({{"x", allValues(BSONType::string)}});
    auto narrowed = original;
    narrowed.setField("a", allValues(BSONType::numberInt));
    const void* copied = getFieldStorage(narrowed);
    ASSERT_NE(copied, getFieldStorage(original));
    narrowed.setField("b", allValues(BSONType::numberInt));
    ASSERT_EQ(getFieldStorage(narrowed), copied);
    narrowed.setField("c", allValues(BSONType::numberInt));
    ASSERT_EQ(getFieldStorage(narrowed), copied);
}

TEST(TypeTest, SetFieldRequiresObjectType) {
    auto type = allValues(BSONType::string);
    ASSERT_TASSERT_CODE(type.setField("x", allValues(BSONType::numberInt)), 13459104);
}

TEST(TypeTest, ObjectsDescribingTheSameValuesCompareEqual) {
    ASSERT_EQ(openObject({{"x", allValues(BSONType::string)}}),
              openObject({{"x", allValues(BSONType::string)}}));
}

TEST(TypeTest, ObjectsDisagreeingOnUnnamedFieldsCompareUnequal) {
    ASSERT_NE(openObject({{"x", allValues(BSONType::string)}}),
              closedObject({{"x", allValues(BSONType::string)}}));
}

TEST(TypeTest, NestedObjectsCompareByTheirFields) {
    ASSERT_EQ(openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}}),
              openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}}));
    ASSERT_NE(openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}}),
              openObject({{"a", openObject({{"b", allValues(BSONType::numberInt)}})}}));
}

TEST(TypeTest, ClosedObjectRendersItsFields) {
    ASSERT_EQ(closedObject({{"x", allValues(BSONType::string)}}).toDebugString(), "{x: string}");
}

TEST(TypeTest, OpenObjectRendersAnEllipsis) {
    ASSERT_EQ(openObject({{"x", allValues(BSONType::string)}}).toDebugString(), "{x: string, ...}");
}

TEST(TypeTest, ClosedObjectWithoutFieldsRendersAsEmptyBraces) {
    ASSERT_EQ(closedObject({}).toDebugString(), "{}");
}

TEST(TypeTest, ObjectFieldsRenderInNameOrder) {
    ASSERT_EQ(
        closedObject({{"b", allValues(BSONType::string)}, {"a", allValues(BSONType::numberInt)}})
            .toDebugString(),
        "{a: int, b: string}");
}

TEST(TypeTest, NestedObjectRendersRecursively) {
    ASSERT_EQ(
        openObject({{"a", closedObject({{"b", allValues(BSONType::numberInt)}})}}).toDebugString(),
        "{a: {b: int}, ...}");
}

TEST(TypeTest, ObjectFieldsAreEscaped) {
    ASSERT_EQ(openObject({{"a ", Type::missing()}, {":\"", Type::missing()}}).toDebugString(),
              "{\":\\\"\": missing, \"a \": missing, ...}");
}

}  // namespace

}  // namespace mongo::pipeline::type_system
