// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/document_tree_generator.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>
#include <string>

namespace mongo::sbe {
namespace {
using value::TagValueOwned;
using value::TypeTags;

BSONObj asBson(const TagValueOwned& value) {
    BSONObjBuilder builder;
    bson::appendValueToBsonObj(builder, "value", value.tag(), value.value());
    return builder.obj();
}

TEST(DocumentTreeGeneratorTest, ScalarValuesPreserveTypeAndOwnTheirPayload) {
    struct Case {
        ScalarSpec spec;
        TypeTags tag;
        BSONObj expected;
    };
    const std::string string = "a string longer than SBE's inline string representation";
    const Case cases[] = {
        {false, TypeTags::Boolean, BSON("value" << false)},
        {true, TypeTags::Boolean, BSON("value" << true)},
        {int32_t{-5}, TypeTags::NumberInt32, BSON("value" << int32_t{-5})},
        {std::numeric_limits<int64_t>::min(),
         TypeTags::NumberInt64,
         BSON("value" << std::numeric_limits<int64_t>::min())},
        {-0.0, TypeTags::NumberDouble, BSON("value" << -0.0)},
        {Decimal128("1.25"), TypeTags::NumberDecimal, BSON("value" << Decimal128("1.25"))},
        {string, TypeTags::StringBig, BSON("value" << string)},
        {std::string("abc"), TypeTags::StringSmall, BSON("value" << "abc")},
        {std::string("a\0b", 3), TypeTags::StringBig, BSON("value" << std::string("a\0b", 3))},
        {NullSpec{}, TypeTags::Null, BSON("value" << BSONNULL)},
        {NothingSpec{}, TypeTags::Nothing, BSONObj{}},
    };
    for (const auto& test : cases) {
        auto input = test.spec;
        auto rendered = renderScalarValue(input);
        input = NothingSpec{};
        ASSERT_EQ(rendered.tag(), test.tag);
        ASSERT_TRUE(test.expected.binaryEqual(asBson(rendered)));
    }
}

TEST(DocumentTreeGeneratorTest, ObjectsOwnNestedValuesAndPreserveFieldOrder) {
    const auto expected =
        BSON("last" << "owned string" << "first" << BSON("nested" << BSON_ARRAY(1 << BSONNULL)));
    auto native = TagValueOwned::nothing();
    auto bson = TagValueOwned::nothing();
    {
        auto input = expected.copy();
        native = renderObjectValue(input);
        bson = renderBsonObjectValue(input);
    }
    ASSERT_EQ(native.tag(), TypeTags::Object);
    ASSERT_EQ(bson.tag(), TypeTags::bsonObject);
    const auto wrapped = BSON("value" << expected);
    ASSERT_TRUE(wrapped.binaryEqual(asBson(native)));
    ASSERT_TRUE(wrapped.binaryEqual(asBson(bson)));
    const auto nested = value::getObjectView(native.value())->getField("first");
    ASSERT_EQ(nested.tag, TypeTags::Object);
}

TEST(DocumentTreeGeneratorTest, ArraysIgnoreNamesAndPreserveValueOrder) {
    auto rendered = TagValueOwned::nothing();
    {
        const auto input =
            BSON("z" << 2 << "a" << BSON("nested" << "owned string") << "b" << BSON_ARRAY(1 << 3));
        rendered = renderArrayValue(input);
    }
    ASSERT_EQ(rendered.tag(), TypeTags::Array);
    const auto expected =
        BSON("value" << BSON_ARRAY(2 << BSON("nested" << "owned string") << BSON_ARRAY(1 << 3)));
    ASSERT_TRUE(expected.binaryEqual(asBson(rendered)));
}

TEST(DocumentTreeGeneratorTest, ArraySetsOwnValuesAndDeduplicateByValue) {
    const std::string string = "an owned string longer than the inline representation";
    auto rendered = TagValueOwned::nothing();
    {
        const auto input =
            BSON("a" << 1 << "b" << 1.0 << "c" << string << "d" << string << "e" << BSONNULL);
        rendered = renderArraySetValue(input);
    }
    ASSERT_EQ(rendered.tag(), TypeTags::ArraySet);
    const auto* set = value::getArraySetView(rendered.value());
    ASSERT_EQ(set->size(), 3u);
    ASSERT_TRUE(set->values().contains({TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)}));
    ASSERT_TRUE(set->values().contains({TypeTags::Null, 0}));
    auto expectedString = TagValueOwned::fromRaw(value::makeNewString(string));
    ASSERT_TRUE(set->values().contains({expectedString.tag(), expectedString.value()}));
}

TEST(DocumentTreeGeneratorTest, EmptyContainers) {
    auto object = renderObjectValue(BSONObj{});
    auto bson = renderBsonObjectValue(BSONObj{});
    auto array = renderArrayValue(BSONObj{});
    auto set = renderArraySetValue(BSONObj{});
    ASSERT_EQ(object.tag(), TypeTags::Object);
    ASSERT_EQ(bson.tag(), TypeTags::bsonObject);
    ASSERT_EQ(array.tag(), TypeTags::Array);
    ASSERT_EQ(set.tag(), TypeTags::ArraySet);
    const auto emptyObject = BSON("value" << BSONObj{});
    const auto emptyArray = BSON("value" << BSONArray{});
    ASSERT_TRUE(emptyObject.binaryEqual(asBson(object)));
    ASSERT_TRUE(emptyObject.binaryEqual(asBson(bson)));
    ASSERT_TRUE(emptyArray.binaryEqual(asBson(array)));
    ASSERT_TRUE(emptyArray.binaryEqual(asBson(set)));
}
}  // namespace
}  // namespace mongo::sbe
