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

TEST(TypeSetTest, HasOnlyTypeIgnoresExtent) {
    ASSERT_TRUE(TypeSet(BSONType::object, Extent::kAll).hasOnlyType(BSONType::object));
    ASSERT_TRUE(TypeSet(BSONType::object, Extent::kSubset).hasOnlyType(BSONType::object));
}

TEST(TypeSetTest, HasOnlyTypeRejectsSetsCoveringOtherTypes) {
    ASSERT_FALSE(TypeSet::any().hasOnlyType(BSONType::object));
    ASSERT_FALSE(TypeSet::never().hasOnlyType(BSONType::object));
    ASSERT_FALSE(TypeSet(BSONType::string, Extent::kAll).hasOnlyType(BSONType::object));
    ASSERT_FALSE(
        unionType(TypeSet(BSONType::object, Extent::kAll), TypeSet(BSONType::array, Extent::kAll))
            .hasOnlyType(BSONType::object));
}

TEST(TypeSetTest, IsNeverHoldsOnlyForEmptySet) {
    ASSERT_TRUE(TypeSet::never().isNever());
    ASSERT_FALSE(TypeSet::any().isNever());
    ASSERT_FALSE(TypeSet(BSONType::object, Extent::kAll).isNever());
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

Type allNumbers() {
    return Type(TypeSet::numericTypes(Extent::kAll));
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

TEST(TypeTest, CanPathBeArrayHandlesNonObjectTypes) {
    ASSERT_FALSE(allValues(BSONType::string).canPathBeArray(FieldRef("x")));
    ASSERT_FALSE(closedObject({}).canPathBeArray(FieldRef("x.y")));
    ASSERT_TRUE(allValues(BSONType::array).canPathBeArray(FieldRef("x")));

    auto mixed =
        unionType(openObject({{"y", allValues(BSONType::array)}}), allValues(BSONType::string));
    ASSERT_TRUE(openObject({{"x", mixed}}).canPathBeArray(FieldRef("x.y")));
}

TEST(TypeTest, CanPathBeArrayStopsAtScalarField) {
    ASSERT_FALSE(openObject({{"x", allValues(BSONType::string)}}).canPathBeArray(FieldRef("x.y")));
}

TEST(TypeTest, CanPathBeArrayStopsAtNever) {
    ASSERT_FALSE(Type::never().canPathBeArray(FieldRef("x.y")));
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

TEST(TypeTest, NarrowFieldIntersectsTheNamedFieldInsteadOfReplacingIt) {
    auto type = narrowField(openObject({{"x", Type::any()}}), "x", allValues(BSONType::numberInt));
    ASSERT_EQ(type.getField("x"), allValues(BSONType::numberInt));
    ASSERT_EQ(narrowField(std::move(type), "x", allValues(BSONType::string)), Type::never());
}

TEST(TypeTest, NarrowFieldRequiresObjectType) {
    auto type = allValues(BSONType::string);
    ASSERT_TASSERT_CODE(narrowField(type, "x", allValues(BSONType::numberInt)), 13459105);
}

TEST(TypeTest, NarrowFieldLeavesItsArgumentUnchanged) {
    auto original = openObject({{"x", Type::any()}});
    auto narrowed = narrowField(original, "x", allValues(BSONType::string));
    ASSERT_EQ(original.getField("x"), Type::any());
    ASSERT_EQ(narrowed.getField("x"), allValues(BSONType::string));
}

TEST(TypeTest, NarrowFieldOnUnsharedShapeReusesIt) {
    auto type = openObject({{"x", allNumbers()}});
    const void* shape = getFieldStorage(type);
    auto narrowed = narrowField(std::move(type), "x", allValues(BSONType::numberInt));
    ASSERT_EQ(getFieldStorage(narrowed), shape);
    ASSERT_EQ(narrowed.getField("x"), allValues(BSONType::numberInt));
}

TEST(TypeTest, NarrowFieldOnUnsharedNestedShapeReusesEveryLevel) {
    auto type = openObject({{"a", openObject({{"b", allNumbers()}})}});
    const void* outer = getFieldStorage(type);
    const void* inner = getFieldStorage(type.getField("a"));
    auto narrowed =
        narrowField(std::move(type), "a", openObject({{"b", allValues(BSONType::numberInt)}}));
    ASSERT_EQ(getFieldStorage(narrowed), outer);
    ASSERT_EQ(getFieldStorage(narrowed.getField("a")), inner);
    ASSERT_EQ(narrowed.getField("a").getField("b"), allValues(BSONType::numberInt));
}

TEST(TypeTest, NarrowFieldOnSharedShapeLeavesTheOtherOwnerUnchanged) {
    auto original = openObject({{"x", allNumbers()}});
    auto shared = original;
    const void* shape = getFieldStorage(original);
    auto narrowed = narrowField(std::move(original), "x", allValues(BSONType::numberInt));
    ASSERT_NE(getFieldStorage(narrowed), shape);
    ASSERT_EQ(getFieldStorage(shared), shape);
    ASSERT_EQ(shared.getField("x"), allNumbers());
}

TEST(TypeTest, NarrowFieldToNoValueAtDepthRemovesTheOuterObjectType) {
    auto type = openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}});
    auto narrowed = narrowField(std::move(type), "a", openObject({{"b", allNumbers()}}));
    ASSERT_EQ(narrowed, Type::never());
}

TEST(TypeTest, UnionOfOpenObjectsIsOpen) {
    auto type = unionType(openObject({{"x", allValues(BSONType::string)}}),
                          openObject({{"x", allValues(BSONType::numberInt)}}));
    ASSERT_TRUE(isOpen(type.getShape_forTest().open));
}

TEST(TypeTest, UnionOfClosedObjectsStaysClosed) {
    auto type = unionType(closedObject({{"x", allValues(BSONType::string)}}),
                          closedObject({{"x", allValues(BSONType::numberInt)}}));
    ASSERT_FALSE(isOpen(type.getShape_forTest().open));
}

TEST(TypeTest, UnionOfClosedAndOpenObjectsIsOpen) {
    auto type = unionType(closedObject({{"x", allValues(BSONType::string)}}),
                          openObject({{"y", allValues(BSONType::numberInt)}}));
    ASSERT_TRUE(isOpen(type.getShape_forTest().open));
}

TEST(TypeTest, UnionOfObjectsUnionsTheTypeOfEachField) {
    auto type = unionType(openObject({{"x", allValues(BSONType::string)}}),
                          openObject({{"x", allValues(BSONType::numberInt)}}));
    ASSERT_EQ(type.getField("x"),
              unionType(allValues(BSONType::string), allValues(BSONType::numberInt)));
}

TEST(TypeTest, UnionAllowsAFieldNamedByOneObjectToBeMissing) {
    auto type = unionType(closedObject({{"x", allValues(BSONType::string)}}),
                          closedObject({{"y", allValues(BSONType::numberInt)}}));
    ASSERT_EQ(type.getField("x"), unionType(allValues(BSONType::string), Type::missing()));
    ASSERT_EQ(type.getField("y"), unionType(allValues(BSONType::numberInt), Type::missing()));
}

TEST(TypeTest, UnionWithBareFullObjectTypeDropsTheShape) {
    auto type = unionType(openObject({{"x", allValues(BSONType::string)}}), object(Extent::kAll));
    ASSERT_TRUE(describesEveryObject(type));
    ASSERT_TRUE(isAll(type.getExtent(BSONType::object)));
}

TEST(TypeTest, UnionWithBareSubsetObjectTypeDropsTheShape) {
    auto type =
        unionType(openObject({{"x", allValues(BSONType::string)}}), object(Extent::kSubset));
    ASSERT_TRUE(describesEveryObject(type));
}

TEST(TypeTest, UnionKeepsTheShapeOfWhicheverSideCoversObjects) {
    auto object = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(unionType(allValues(BSONType::array), object),
              unionType(object, allValues(BSONType::array)));
    ASSERT_EQ(unionType(allValues(BSONType::array), object).getField("x"),
              allValues(BSONType::string));
}

TEST(TypeTest, UnionOfClosedObjectsWithoutFieldsNamesNoField) {
    auto type = unionType(closedObject({}), closedObject({}));
    ASSERT_EQ(type, closedObject({}));
    ASSERT_TRUE(type.getShape_forTest().fields.empty());
}

TEST(TypeTest, IntersectionOfClosedObjectsWithoutFieldsNamesNoField) {
    auto type = intersectType(closedObject({}), closedObject({}));
    ASSERT_EQ(type, closedObject({}));
    ASSERT_TRUE(type.getShape_forTest().fields.empty());
}

TEST(TypeTest, UnionWithATypeNotCoveringObjectsKeepsTheShape) {
    auto object = openObject({{"x", allValues(BSONType::string)}});
    auto type = unionType(object, allValues(BSONType::array));
    ASSERT_EQ(type.getField("x"), allValues(BSONType::string));
    ASSERT_TRUE(type.hasType(BSONType::array));
}

TEST(TypeTest, UnionOfNestedObjectsRecurses) {
    auto type = unionType(openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}}),
                          openObject({{"a", openObject({{"b", allValues(BSONType::numberInt)}})}}));
    ASSERT_EQ(type.getField("a").getField("b"),
              unionType(allValues(BSONType::string), allValues(BSONType::numberInt)));
}

TEST(TypeTest, IntersectionOfOpenObjectsIsOpen) {
    auto type = intersectType(openObject({{"x", allValues(BSONType::string)}}),
                              openObject({{"y", allValues(BSONType::numberInt)}}));
    ASSERT_TRUE(isOpen(type.getShape_forTest().open));
    ASSERT_EQ(type.getField("x"), allValues(BSONType::string));
    ASSERT_EQ(type.getField("y"), allValues(BSONType::numberInt));
}

TEST(TypeTest, IntersectionOfOpenAndClosedObjectsIsClosed) {
    auto type =
        intersectType(openObject({{"x", unionType(allValues(BSONType::string), Type::missing())}}),
                      closedObject({{"x", allValues(BSONType::string)}}));
    ASSERT_FALSE(isOpen(type.getShape_forTest().open));
    ASSERT_EQ(type.getField("x"), allValues(BSONType::string));
}

TEST(TypeTest, IntersectionOfObjectsIntersectsTheTypeOfEachField) {
    auto type = intersectType(
        openObject({{"x", unionType(allValues(BSONType::string), allValues(BSONType::numberInt))}}),
        openObject({{"x", allValues(BSONType::string)}}));
    ASSERT_EQ(type.getField("x"), allValues(BSONType::string));
}

TEST(TypeTest, IntersectionWithAClosedObjectRemovingARequiredFieldRemovesTheObjectType) {
    auto type = intersectType(openObject({{"x", allValues(BSONType::string)}}),
                              closedObject({{"y", allValues(BSONType::numberInt)}}));
    ASSERT_FALSE(type.hasType(BSONType::object));
}

TEST(TypeTest, IntersectionOfClosedObjectsNamingDifferentFieldsRemovesTheObjectType) {
    auto type = intersectType(closedObject({{"x", allValues(BSONType::string)}}),
                              closedObject({{"y", allValues(BSONType::numberInt)}}));
    ASSERT_EQ(type, Type::never());
}

TEST(TypeTest, IntersectionWithBareObjectTypeKeepsTheShape) {
    auto type = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(intersectType(type, object(Extent::kAll)), type);
}

TEST(TypeTest, IntersectionRemovingTheObjectTypeIsNotTheIntersectionOfTheTypeSets) {
    auto lhs = openObject({{"x", allValues(BSONType::string)}});
    auto rhs = closedObject({{"y", allValues(BSONType::numberInt)}});
    ASSERT_TRUE(intersectType(lhs.getTypeSet(), rhs.getTypeSet()).hasType(BSONType::object));
    ASSERT_FALSE(intersectType(lhs, rhs).hasType(BSONType::object));
}

TEST(TypeTest, IntersectionOfNestedObjectsRecurses) {
    auto type =
        intersectType(openObject({{"a",
                                   openObject({{"b",
                                                unionType(allValues(BSONType::string),
                                                          allValues(BSONType::numberInt))}})}}),
                      openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}}));
    ASSERT_EQ(type.getField("a").getField("b"), allValues(BSONType::string));
}

TEST(TypeTest, ComplementDropsTheShape) {
    auto type = complement(openObject({{"x", allValues(BSONType::string)}}));
    ASSERT_TRUE(describesEveryObject(type));
}

TEST(TypeTest, ComplementLeavesObjectsPossible) {
    auto type = complement(openObject({{"x", allValues(BSONType::string)}}));
    ASSERT_TRUE(type.hasType(BSONType::object));
    ASSERT_TRUE(isSubset(type.getExtent(BSONType::object)));
}

TEST(TypeTest, ComplementOfComplementDoesNotRestoreTheShape) {
    auto object = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_NE(complement(complement(object)), object);
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

TEST(TypeTest, ObjectRendersInSortOrderWithinAUnion) {
    auto type =
        unionType(closedObject({{"x", allValues(BSONType::string)}}), allValues(BSONType::array));
    ASSERT_EQ(type.toDebugString(), "{x: string}|array");
}

TEST(TypeTest, ObjectWithKnownFieldsAloneRendersItsFields) {
    auto type = intersectType(openObject({{"x", allValues(BSONType::string)}}),
                              complement(allValues(BSONType::array)));
    ASSERT_EQ(type.toDebugString(), "{x: string, ...}");
}

TEST(TypeTest, ObjectWithKnownFieldsAmongEveryOtherTypeRendersAsNegation) {
    auto type = unionType(complement(object(Extent::kAll)), openObject({{"y", Type::missing()}}));
    // The syntax means "not an object or (if an object) an object of this shape".
    // This is shorter than the positive case, which is to construct a union of all BSON types,
    // with the shape in place of 'object'.
    ASSERT_EQ(type.toDebugString(), "~object|{y: missing, ...}");
}

TEST(TypeTest, NegationBesideObjectWithKnownFieldsKeepsSubsetSuffix) {
    auto nonObjects =
        complement(unionType(object(Extent::kAll), Type(BSONType::array, Extent::kSubset)));
    auto type = unionType(nonObjects, openObject({{"y", Type::missing()}}));
    // The syntax extends to multiple excluded types, same as normal union.
    ASSERT_EQ(type.toDebugString(), "~(object|array(S))|{y: missing, ...}");
}

TEST(TypeTest, NegationBesideObjectWithKnownFieldsRendersInSortOrder) {
    auto nonObjects = complement(unionType(object(Extent::kAll), allValues(BSONType::null)));
    auto type = unionType(nonObjects, openObject({{"y", Type::missing()}}));
    ASSERT_EQ(type.toDebugString(), "~(null|object)|{y: missing, ...}");
}

TEST(TypeTest, NestedNegationBesideObjectWithKnownFieldsRendersInsideField) {
    auto field = unionType(complement(object(Extent::kAll)), openObject({{"y", Type::missing()}}));
    ASSERT_EQ(openObject({{"a", field}}).toDebugString(), "{a: ~object|{y: missing, ...}, ...}");
}

TEST(TypeTest, UnionOfTypesWithoutKnownFieldsHasNoShape) {
    auto type = unionType(object(Extent::kAll), Type::any());
    ASSERT_TRUE(describesEveryObject(type));
    ASSERT_EQ(type, Type::any());
}

TEST(TypeTest, IntersectionOfTypesWithoutKnownFieldsHasNoShape) {
    auto type = intersectType(object(Extent::kAll), Type::any());
    ASSERT_TRUE(describesEveryObject(type));
    ASSERT_EQ(type, object(Extent::kAll));
}

TEST(TypeTest, UnionOfAnObjectWithItselfIsThatObject) {
    auto object = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(unionType(object, object), object);
}

TEST(TypeTest, IntersectionOfAnObjectWithItselfIsThatObject) {
    auto object = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(intersectType(object, object), object);
}

TEST(TypeTest, UnionOfObjectWithItselfKeepsShape) {
    auto object = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(getFieldStorage(unionType(object, object)), getFieldStorage(object));
}

TEST(TypeTest, IntersectionOfObjectWithItselfKeepsShape) {
    auto object = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(getFieldStorage(intersectType(object, object)), getFieldStorage(object));
}

TEST(TypeTest, UnionOfObjectsIsCommutative) {
    auto lhs = closedObject({{"x", allValues(BSONType::string)}});
    auto rhs = openObject({{"y", allValues(BSONType::numberInt)}});
    ASSERT_EQ(unionType(lhs, rhs), unionType(rhs, lhs));
}

TEST(TypeTest, IntersectionOfObjectsIsCommutative) {
    auto lhs = openObject({{"x", allValues(BSONType::string)}});
    auto rhs = openObject({{"y", allValues(BSONType::numberInt)}});
    ASSERT_EQ(intersectType(lhs, rhs), intersectType(rhs, lhs));
}

TEST(TypeTest, IntersectionWithAnUnsharedShapeReusesIt) {
    auto lhs = openObject({{"x", allNumbers()}, {"y", allValues(BSONType::string)}});
    const void* shape = getFieldStorage(lhs);
    auto result =
        intersectType(std::move(lhs), openObject({{"x", allValues(BSONType::numberInt)}}));
    ASSERT_EQ(getFieldStorage(result), shape);
    ASSERT_EQ(result.getField("x"), allValues(BSONType::numberInt));
    ASSERT_EQ(result.getField("y"), allValues(BSONType::string));
}

TEST(TypeTest, IntersectionReusesWhicheverSideIsUnshared) {
    auto lhs = openObject({{"x", allNumbers()}, {"y", allValues(BSONType::string)}});
    auto shared = lhs;
    auto rhs = openObject({{"x", allValues(BSONType::numberInt)}});
    const void* rhsShape = getFieldStorage(rhs);
    auto result = intersectType(std::move(lhs), std::move(rhs));
    ASSERT_EQ(getFieldStorage(result), rhsShape);
    ASSERT_EQ(result.getField("x"), allValues(BSONType::numberInt));
    ASSERT_EQ(result.getField("y"), allValues(BSONType::string));
}

TEST(TypeTest, IntersectionWithASharedShapeLeavesTheOtherOwnerUnchanged) {
    auto lhs = openObject({{"x", allNumbers()}, {"y", allValues(BSONType::string)}});
    auto shared = lhs;
    const void* shape = getFieldStorage(lhs);
    auto result =
        intersectType(std::move(lhs), openObject({{"x", allValues(BSONType::numberInt)}}));
    ASSERT_NE(getFieldStorage(result), shape);
    ASSERT_EQ(getFieldStorage(shared), shape);
    ASSERT_EQ(shared.getField("x"), allNumbers());
}

TEST(TypeTest, UnionWithAnUnsharedShapeReusesIt) {
    auto lhs = openObject({{"x", allValues(BSONType::numberInt)}, {"y", allNumbers()}});
    const void* shape = getFieldStorage(lhs);
    auto result = unionType(std::move(lhs), openObject({{"y", allValues(BSONType::string)}}));
    ASSERT_EQ(getFieldStorage(result), shape);
    ASSERT_EQ(result.getField("x"), Type::any());
    ASSERT_EQ(result.getField("y"), unionType(allNumbers(), allValues(BSONType::string)));
}

TEST(TypeTest, IntersectionAgreesWhetherOrNotTheShapeIsShared) {
    auto rhs =
        openObject({{"x", allValues(BSONType::numberInt)}, {"z", allValues(BSONType::date)}});
    auto unshared = openObject({{"x", allNumbers()}, {"y", allValues(BSONType::string)}});
    auto shared = openObject({{"x", allNumbers()}, {"y", allValues(BSONType::string)}});
    auto otherOwner = shared;
    ASSERT_EQ(intersectType(std::move(unshared), rhs), intersectType(std::move(shared), rhs));
}

TEST(TypeTest, UnionAgreesWhetherOrNotTheShapeIsShared) {
    auto rhs =
        openObject({{"x", allValues(BSONType::numberInt)}, {"z", allValues(BSONType::date)}});
    auto unshared = openObject({{"x", allNumbers()}, {"y", allValues(BSONType::string)}});
    auto shared = openObject({{"x", allNumbers()}, {"y", allValues(BSONType::string)}});
    auto otherOwner = shared;
    ASSERT_EQ(unionType(std::move(unshared), rhs), unionType(std::move(shared), rhs));
}

TEST(TypeTest, ResolveFieldAccessOnObjectReadsTheField) {
    auto input = openObject({{"x", allValues(BSONType::string)}});
    ASSERT_EQ(resolveFieldAccess(input, "x"), allValues(BSONType::string));
    ASSERT_EQ(resolveFieldAccess(input, "y"), Type::any());
    ASSERT_EQ(resolveFieldAccess(closedObject({}), "x"), Type::missing());
}

TEST(TypeTest, ResolveFieldAccessOnScalarIsMissing) {
    ASSERT_EQ(resolveFieldAccess(allValues(BSONType::string), "x"), Type::missing());
}

TEST(TypeTest, ResolveFieldAccessOnPossibleArrayIsAny) {
    auto input =
        unionType(openObject({{"x", allValues(BSONType::string)}}), allValues(BSONType::array));
    ASSERT_EQ(resolveFieldAccess(input, "x"), Type::any());
}

TEST(TypeTest, ResolveFieldAccessOnMixedObjectAndScalarKeepsMissing) {
    auto input =
        unionType(openObject({{"x", allValues(BSONType::string)}}), allValues(BSONType::numberInt));
    ASSERT_EQ(resolveFieldAccess(input, "x"),
              unionType(allValues(BSONType::string), Type::missing()));
}

TEST(TypeTest, ResolveFieldAccessOnNestedObject) {
    auto input = openObject({{"a",
                              closedObject({{"b", allValues(BSONType::string)},
                                            {"items", allValues(BSONType::array)}})}});
    auto a = resolveFieldAccess(input, "a");
    ASSERT_EQ(resolveFieldAccess(a, "b"), allValues(BSONType::string));
    ASSERT_EQ(resolveFieldAccess(a, "missing"), Type::missing());
    ASSERT_EQ(resolveFieldAccess(a, "items"), allValues(BSONType::array));
    ASSERT_EQ(resolveFieldAccess(resolveFieldAccess(a, "items"), "x"), Type::any());
}

TEST(TypeTest, ResolveFieldAccessRejectsNever) {
    ASSERT_TASSERT_CODE(resolveFieldAccess(Type::never(), "x"), 13459501);
}

}  // namespace

}  // namespace mongo::pipeline::type_system
