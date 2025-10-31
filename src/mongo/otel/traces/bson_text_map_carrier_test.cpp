/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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

}  // namespace
}  // namespace traces
}  // namespace otel
}  // namespace mongo
