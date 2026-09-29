// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/type.h"

#include "mongo/unittest/unittest.h"

namespace mongo::pipeline::type_system {

namespace {

constexpr std::array kAllTypes{
    BSONType::minKey,        BSONType::eoo,       BSONType::numberDouble, BSONType::string,
    BSONType::object,        BSONType::array,     BSONType::binData,      BSONType::undefined,
    BSONType::oid,           BSONType::boolean,   BSONType::date,         BSONType::null,
    BSONType::regEx,         BSONType::dbRef,     BSONType::code,         BSONType::symbol,
    BSONType::codeWScope,    BSONType::numberInt, BSONType::timestamp,    BSONType::numberLong,
    BSONType::numberDecimal, BSONType::maxKey};

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

}  // namespace

}  // namespace mongo::pipeline::type_system
