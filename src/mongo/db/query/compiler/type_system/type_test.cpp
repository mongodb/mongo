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

}  // namespace

}  // namespace mongo::pipeline::type_system
