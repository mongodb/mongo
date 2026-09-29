// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/type.h"

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

}  // namespace

}  // namespace mongo::pipeline::type_system
