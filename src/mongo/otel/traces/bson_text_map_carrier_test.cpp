// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/bson_text_map_carrier.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace otel {
namespace traces {
namespace {

constexpr auto keyA = "keyA";
constexpr auto valueA = "valueA";
constexpr auto keyB = "keyB";
constexpr auto valueB = "valueB";

TEST(BSONTextMapCarrier, SetAndGetOne) {
    BSONTextMapCarrier carrier;
    carrier.Set(keyA, valueA);
    ASSERT_EQ(carrier.Get(keyA), valueA);
}

TEST(BSONTextMapCarrier, SetAndGetMany) {
    BSONTextMapCarrier carrier;
    carrier.Set(keyA, valueA);
    carrier.Set(keyB, valueB);
    ASSERT_EQ(carrier.Get(keyA), valueA);
    ASSERT_EQ(carrier.Get(keyB), valueB);
}

TEST(BSONTextMapCarrier, GetExistingKey) {
    BSONTextMapCarrier carrier(BSON(keyA << valueA));
    ASSERT_EQ(carrier.Get(keyA), valueA);
}

TEST(BSONTextMapCarrier, GetMissingKey) {
    BSONTextMapCarrier carrier;
    ASSERT_EQ(carrier.Get(keyA), kMissingKeyReturnValue);
}

TEST(BSONTextMapCarrier, SetExistingKey) {
    constexpr auto newValue = "newValue";
    BSONTextMapCarrier carrier(BSON(keyA << valueA));
    carrier.Set(keyA, newValue);
    ASSERT_EQ(carrier.Get(keyA), newValue);
}

TEST(BSONTextMapCarrier, SetAndReturnBSON) {
    constexpr auto newValue = "newValue";
    BSONTextMapCarrier carrier(BSON(keyA << valueA << keyB << valueB));
    carrier.Set(keyA, newValue);
    auto bson = carrier.toBSON();
    ASSERT_EQ(bson.getStringField(keyA), newValue);
    ASSERT_EQ(bson.getStringField(keyB), valueB);
}

TEST(BSONTextMapCarrier, SetNewKeyAndReturnBSON) {
    BSONTextMapCarrier carrier;
    carrier.Set(keyA, valueA);
    auto bson = carrier.toBSON();
    ASSERT_EQ(bson.getStringField(keyA), valueA);
}

TEST(BSONTextMapCarrier, KeysReturnsFalseIfCallbackReturnsFalse) {
    BSONTextMapCarrier carrier(BSON(keyA << valueA));
    ASSERT_FALSE(carrier.Keys([](OtelStringView) { return false; }));
}

TEST(BSONTextMapCarrier, GetAllKeys) {
    BSONTextMapCarrier carrier(BSON(keyA << valueA << keyB << valueB));
    auto keys = getKeySet(carrier);
    ASSERT_EQ(keys.size(), 2);
    ASSERT_TRUE(keys.contains(keyA));
    ASSERT_TRUE(keys.contains(keyB));
}

TEST(BSONTextMapCarrier, GetAllKeysWithSetKeys) {
    BSONTextMapCarrier carrier(BSON(keyA << valueA));
    carrier.Set(keyB, valueB);
    auto keys = getKeySet(carrier);
    ASSERT_EQ(keys.size(), 2);
    ASSERT_TRUE(keys.contains(keyA));
    ASSERT_TRUE(keys.contains(keyB));
}

TEST(BSONTextMapCarrier, CarrierOutlivesSetValue) {
    BSONTextMapCarrier carrier;
    {
        std::string key = "temporaryKey";
        std::string value = "temporaryValue";
        carrier.Set(key, value);
    }
    ASSERT_EQ(carrier.Get("temporaryKey"), "temporaryValue");
}

TEST(BSONTextMapCarrier, NonStringFieldsIgnored) {
    BSONTextMapCarrier carrier(BSON("uuid" << UUID::gen() << keyA << valueA));
    auto keys = getKeySet(carrier);
    ASSERT_EQ(keys.size(), 1);
    ASSERT_TRUE(keys.contains(keyA));
    ASSERT_EQ(carrier.Get(keyA), valueA);
    ASSERT_EQ(carrier.Get("uuid"), kMissingKeyReturnValue);
}

TEST(BSONTextMapCarrier, ConstructorWithTelemetryContextSection) {
    auto section = TelemetryContextSection{
        OtelContextSection{"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"}};
    auto carrier = BSONTextMapCarrier{section};
    EXPECT_EQ(carrier.Get(BSONTextMapCarrier::kTraceParentKey),
              "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
}

TEST(BSONTextMapCarrier, SetAndGet) {
    constexpr auto keyA = "keyA";
    constexpr auto valueA = "valueA";
    constexpr auto keyB = "keyB";
    constexpr auto valueB = "valueB";

    auto carrier = BSONTextMapCarrier{};
    carrier.Set(keyA, valueA);
    EXPECT_EQ(carrier.Get(keyA), valueA);
    carrier.Set(keyB, valueB);
    EXPECT_EQ(carrier.Get(keyB), valueB);
}

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo
