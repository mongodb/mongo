// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/container_oplog_entry_serialization.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::repl {
namespace {
using namespace std::literals::string_view_literals;
// Round trip tests
TEST(ContainerOplogEntrySerializationTest, IntKeyRoundTrip) {
    ContainerKey key{int64_t{42}};
    EXPECT_TRUE(key.isIntKey());
    EXPECT_FALSE(key.isArrayKey());
    EXPECT_FALSE(key.isBytesKey());
    EXPECT_EQ(key.getIntKey(), 42);
    EXPECT_ANY_THROW(key.getBytesKey());
    EXPECT_ANY_THROW(key.getArrayKey());
}

// ContainerKey and ContainerVal expose the "bytes" and "array" alternatives under different member
// names (and ContainerVal has no int alternative). These overloads give the type-parameterized
// tests below a uniform vocabulary so a single test body exercises both types.
bool isBytes(const ContainerKey& c) {
    return c.isBytesKey();
}
bool isBytes(const ContainerVal& c) {
    return !c.isArrayVal();
}
bool isArray(const ContainerKey& c) {
    return c.isArrayKey();
}
bool isArray(const ContainerVal& c) {
    return c.isArrayVal();
}
std::span<const char> getBytes(const ContainerKey& c) {
    return c.getBytesKey();
}
std::span<const char> getBytes(const ContainerVal& c) {
    return c.data();
}
auto getArray(const ContainerKey& c) {
    return c.getArrayKey();
}
auto getArray(const ContainerVal& c) {
    return c.getArrayVal();
}

template <typename T>
class TypedContainerOplogEntrySerializationTest : public testing::Test {};
TYPED_TEST_SUITE_P(TypedContainerOplogEntrySerializationTest);

TYPED_TEST_P(TypedContainerOplogEntrySerializationTest, BytesRoundTrip) {
    TypeParam c{"Hello World"};
    EXPECT_TRUE(isBytes(c));
    EXPECT_FALSE(isArray(c));
    EXPECT_EQ(std::string_view(getBytes(c).data(), getBytes(c).size()), "Hello World\0"sv);
    EXPECT_ANY_THROW(getArray(c));
}

TYPED_TEST_P(TypedContainerOplogEntrySerializationTest, ArrayRoundTrip) {
    const auto sv0 = "Hello World\0"sv;
    const auto sv1 = "World Hello\0"sv;
    const std::vector<std::span<const char>> arr{std::span<const char>{sv0.data(), sv0.size()},
                                                 std::span<const char>{sv1.data(), sv1.size()}};
    TypeParam c{arr};
    EXPECT_FALSE(isBytes(c));
    EXPECT_TRUE(isArray(c));
    auto got = getArray(c);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(std::string_view(got[0].data(), got[0].size()), sv0);
    EXPECT_EQ(std::string_view(got[1].data(), got[1].size()), sv1);
    EXPECT_ANY_THROW(getBytes(c));
}

// Serialize a bytes value, then parse it back out of the resulting BSON. Also exercises the single
// BinData branch of parse().
TYPED_TEST_P(TypedContainerOplogEntrySerializationTest, BytesSerializeRoundTrip) {
    TypeParam original{"Hello World"};
    BSONObjBuilder builder;
    original.serialize("f", &builder);
    const auto obj = builder.obj();

    const auto parsed = TypeParam::parse(obj["f"]);
    EXPECT_TRUE(isBytes(parsed));
    EXPECT_FALSE(isArray(parsed));
    EXPECT_EQ(std::string_view(getBytes(parsed).data(), getBytes(parsed).size()),
              "Hello World\0"sv);
}

// Serialize an array, then parse it back out of the resulting BSON. Also exercises the array branch
// of parse() and the appendBinDataArray helper.
TYPED_TEST_P(TypedContainerOplogEntrySerializationTest, ArraySerializeRoundTrip) {
    const auto sv0 = "Hello World\0"sv;
    const auto sv1 = "World Hello\0"sv;
    std::vector<std::span<const char>> arr{std::span<const char>{sv0.data(), sv0.size()},
                                           std::span<const char>{sv1.data(), sv1.size()}};
    TypeParam original{arr};
    BSONObjBuilder builder;
    original.serialize("f", &builder);
    const auto obj = builder.obj();

    const auto parsed = TypeParam::parse(obj["f"]);
    EXPECT_TRUE(isArray(parsed));
    EXPECT_FALSE(isBytes(parsed));
    auto got = getArray(parsed);
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(std::string_view(got[0].data(), got[0].size()), sv0);
    EXPECT_EQ(std::string_view(got[1].data(), got[1].size()), sv1);
}

REGISTER_TYPED_TEST_SUITE_P(TypedContainerOplogEntrySerializationTest,
                            BytesRoundTrip,
                            ArrayRoundTrip,
                            BytesSerializeRoundTrip,
                            ArraySerializeRoundTrip);

using ContainerTypes = testing::Types<ContainerKey, ContainerVal>;
INSTANTIATE_TYPED_TEST_SUITE_P(KeyAndVal,
                               TypedContainerOplogEntrySerializationTest,
                               ContainerTypes);

TEST(ContainerOplogEntrySerializationTest, SingleInsertFormat) {
    // "o": { "k": <NumberLong 123>, "v": BinData("abc") }
    const auto singleEntry = BSON("k" << 123LL << "v" << BSONBinData("abc", 3, BinDataGeneral));
    const auto parsed =
        ContainerInsertOplogEntryO::parse(singleEntry, IDLParserContext("SingleInsertFormat"));

    EXPECT_TRUE(parsed.getKey().isIntKey());
    EXPECT_EQ(parsed.getKey().getIntKey(), 123);

    ASSERT_TRUE(parsed.getValue().has_value());
    EXPECT_FALSE(parsed.getValue()->isArrayVal());
    const auto value = parsed.getValue()->data();
    EXPECT_EQ(std::string_view(value.data(), value.size()), "abc"sv);
}

TEST(ContainerOplogEntrySerializationTest, RangeInsertForIntKeyedContainer) {
    // "o": { "k": <NumberLong 123>, "v": [BinData("a"), BinData("b"), BinData("c")] }
    // A range insert into an integer-keyed container: 'k' is the first key.
    const auto rangeInsertIntKeyed =
        BSON("k" << 123LL << "v"
                 << BSON_ARRAY(BSONBinData("a", 1, BinDataGeneral)
                               << BSONBinData("b", 1, BinDataGeneral)
                               << BSONBinData("c", 1, BinDataGeneral)));
    const auto parsed = ContainerInsertOplogEntryO::parse(
        rangeInsertIntKeyed, IDLParserContext("RangeInsertForIntKeyedContainer"));

    EXPECT_TRUE(parsed.getKey().isIntKey());
    EXPECT_EQ(parsed.getKey().getIntKey(), 123);

    ASSERT_TRUE(parsed.getValue().has_value());
    EXPECT_TRUE(parsed.getValue()->isArrayVal());
    const auto values = parsed.getValue()->getArrayVal();
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(std::string_view(values[0].data(), values[0].size()), "a"sv);
    EXPECT_EQ(std::string_view(values[1].data(), values[1].size()), "b"sv);
    EXPECT_EQ(std::string_view(values[2].data(), values[2].size()), "c"sv);
}

TEST(ContainerOplogEntrySerializationTest, RangeInsertForStringKeyedContainer) {
    // "o": { "k": [BinData("a"), BinData("b"), BinData("c")] }
    // Key-array insert into an array of keys with empty values.
    const auto rangeInsertStringKeyed =
        BSON("k" << BSON_ARRAY(BSONBinData("a", 1, BinDataGeneral)
                               << BSONBinData("b", 1, BinDataGeneral)
                               << BSONBinData("c", 1, BinDataGeneral)));
    const auto parsed = ContainerInsertOplogEntryO::parse(
        rangeInsertStringKeyed, IDLParserContext("RangeInsertForStringKeyedContainer"));

    EXPECT_TRUE(parsed.getKey().isArrayKey());
    const auto keys = parsed.getKey().getArrayKey();
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(std::string_view(keys[0].data(), keys[0].size()), "a"sv);
    EXPECT_EQ(std::string_view(keys[1].data(), keys[1].size()), "b"sv);
    EXPECT_EQ(std::string_view(keys[2].data(), keys[2].size()), "c"sv);

    EXPECT_FALSE(parsed.getValue().has_value());
}

// Int keys are ContainerKey-only, so this round trip is not part of the typed suite.
TEST(ContainerOplogEntrySerializationTest, IntKeySerializeRoundTrip) {
    ContainerKey original{int64_t{42}};
    BSONObjBuilder builder;
    original.serialize("k", &builder);
    const auto obj = builder.obj();

    const auto parsed = ContainerKey::parse(obj["k"]);
    EXPECT_TRUE(parsed.isIntKey());
    EXPECT_EQ(parsed.getIntKey(), 42);
}

// Negative parse cases.
TEST(ContainerOplogEntrySerializationTest, ContainerKeyParseRejectsUnexpectedType) {
    // A key must be NumberLong, BinData, or an array; a string is none of those.
    const auto obj = BSON("k" << "not a key");
    ASSERT_THROWS_CODE(ContainerKey::parse(obj["k"]), DBException, 12270900);
}

TEST(ContainerOplogEntrySerializationTest, ContainerValParseRejectsUnexpectedType) {
    // A value must be BinData or an array; a string is neither.
    const auto obj = BSON("v" << "not a value");
    ASSERT_THROWS_CODE(ContainerVal::parse(obj["v"]), DBException, ErrorCodes::TypeMismatch);
}

TEST(ContainerOplogEntrySerializationTest, ContainerKeyParseRejectsNonBinDataArrayElement) {
    const auto obj = BSON("k" << BSON_ARRAY(1 << 2));
    ASSERT_THROWS_CODE(ContainerKey::parse(obj["k"]), DBException, ErrorCodes::TypeMismatch);
}

TEST(ContainerOplogEntrySerializationTest, ContainerValParseRejectsNonBinDataArrayElement) {
    const auto obj = BSON("v" << BSON_ARRAY(1 << 2));
    ASSERT_THROWS_CODE(ContainerVal::parse(obj["v"]), DBException, ErrorCodes::TypeMismatch);
}

TEST(ContainerOplogEntrySerializationTest, ContainerKeyIsPacked) {
    const char k[] = "K1";
    // Only an array holds more than one key.
    ASSERT_TRUE(
        ContainerKey::isPacked(BSON("k" << BSON_ARRAY(BSONBinData(k, 2, BinDataGeneral)))["k"]));
    ASSERT_FALSE(ContainerKey::isPacked(BSON("k" << BSONBinData(k, 2, BinDataGeneral))["k"]));
    ASSERT_FALSE(ContainerKey::isPacked(BSON("k" << int64_t{7})["k"]));

    // An array of one is still the packed encoding, and an absent key is not packed.
    ASSERT_TRUE(ContainerKey::isPacked(BSON("k" << BSONArray())["k"]));
    ASSERT_FALSE(ContainerKey::isPacked(BSONObj()["k"]));
}

TEST(ContainerOplogEntrySerializationTest, ContainerValIsPacked) {
    const char v[] = "V";
    ASSERT_TRUE(
        ContainerVal::isPacked(BSON("v" << BSON_ARRAY(BSONBinData(v, 1, BinDataGeneral)))["v"]));
    ASSERT_FALSE(ContainerVal::isPacked(BSON("v" << BSONBinData(v, 1, BinDataGeneral))["v"]));

    // An absent value is not packed, which is the common shape for a bytes-keyed range insert.
    ASSERT_FALSE(ContainerVal::isPacked(BSONObj()["v"]));
}

}  // namespace
}  // namespace mongo::repl
