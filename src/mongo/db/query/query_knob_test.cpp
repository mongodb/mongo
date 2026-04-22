/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_knob.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/synchronized_value.h"

#include <algorithm>

namespace mongo {
namespace {

AtomicWord<int> testAtomicInt{42};
AtomicWord<double> testAtomicDouble{3.14};
AtomicWord<bool> testAtomicBool{true};
AtomicWord<long long> testAtomicLongLong{100LL};
synchronized_value<QueryFrameworkControlEnum> testSyncEnum{
    QueryFrameworkControlEnum::kTrySbeEngine};

}  // namespace

namespace test_knobs {

inline QueryKnob<int> testIntKnob{"testIntKnob", &readGlobalValue<testAtomicInt>};
inline QueryKnob<double> testDoubleKnob{"testDoubleKnob", &readGlobalValue<testAtomicDouble>};
inline QueryKnob<bool> testBoolKnob{"testBoolKnob", &readGlobalValue<testAtomicBool>};
inline QueryKnob<long long> testLLKnob{"testLLKnob", &readGlobalValue<testAtomicLongLong>};
inline QueryKnob<QueryFrameworkControlEnum> testEnumKnob{"testEnumKnob",
                                                         &readGlobalValue<testSyncEnum>};

}  // namespace test_knobs

namespace {

QueryKnobBase* findByName(StringData name) {
    auto& descriptors = QueryKnobDescriptorSet::get().knobs();
    auto it = std::find_if(
        descriptors.begin(), descriptors.end(), [&](auto* d) { return name == d->paramName; });
    return it != descriptors.end() ? *it : nullptr;
}

TEST(QueryKnobTest, SyntheticKnobsSelfRegister) {
    ASSERT_EQ(findByName("testIntKnob"_sd), &test_knobs::testIntKnob);
    ASSERT_EQ(findByName("testDoubleKnob"_sd), &test_knobs::testDoubleKnob);
    ASSERT_EQ(findByName("testBoolKnob"_sd), &test_knobs::testBoolKnob);
    ASSERT_EQ(findByName("testLLKnob"_sd), &test_knobs::testLLKnob);
    ASSERT_EQ(findByName("testEnumKnob"_sd), &test_knobs::testEnumKnob);
}

TEST(QueryKnobTest, SentinelIndex) {
    ASSERT_EQ(test_knobs::testIntKnob.index, ~size_t{0});
    ASSERT_EQ(test_knobs::testDoubleKnob.index, ~size_t{0});
    ASSERT_EQ(test_knobs::testBoolKnob.index, ~size_t{0});
    ASSERT_EQ(test_knobs::testLLKnob.index, ~size_t{0});
    ASSERT_EQ(test_knobs::testEnumKnob.index, ~size_t{0});
}

TEST(QueryKnobTest, ReadGlobalInt) {
    auto val = test_knobs::testIntKnob.readGlobal();
    ASSERT(std::holds_alternative<int>(val));
    ASSERT_EQ(std::get<int>(val), 42);
}

TEST(QueryKnobTest, ReadGlobalDouble) {
    auto val = test_knobs::testDoubleKnob.readGlobal();
    ASSERT(std::holds_alternative<double>(val));
    ASSERT_APPROX_EQUAL(std::get<double>(val), 3.14, 1e-9);
}

TEST(QueryKnobTest, ReadGlobalBool) {
    auto val = test_knobs::testBoolKnob.readGlobal();
    ASSERT(std::holds_alternative<bool>(val));
    ASSERT_EQ(std::get<bool>(val), true);
}

TEST(QueryKnobTest, ReadGlobalLongLong) {
    auto val = test_knobs::testLLKnob.readGlobal();
    ASSERT(std::holds_alternative<long long>(val));
    ASSERT_EQ(std::get<long long>(val), 100LL);
}

TEST(QueryKnobTest, ReadGlobalSynchronizedEnum) {
    auto val = test_knobs::testEnumKnob.readGlobal();
    ASSERT(std::holds_alternative<int>(val));
    ASSERT_EQ(std::get<int>(val), static_cast<int>(QueryFrameworkControlEnum::kTrySbeEngine));
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesInt) {
    testAtomicInt.store(999);
    auto val = test_knobs::testIntKnob.readGlobal();
    ASSERT_EQ(std::get<int>(val), 999);
    testAtomicInt.store(42);  // Restore.
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesDouble) {
    testAtomicDouble.store(6.28);
    auto val = test_knobs::testDoubleKnob.readGlobal();
    ASSERT_APPROX_EQUAL(std::get<double>(val), 6.28, 1e-9);
    testAtomicDouble.store(3.14);  // Restore.
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesBool) {
    testAtomicBool.store(false);
    auto val = test_knobs::testBoolKnob.readGlobal();
    ASSERT_EQ(std::get<bool>(val), false);
    testAtomicBool.store(true);  // Restore.
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesLongLong) {
    testAtomicLongLong.store(9999LL);
    auto val = test_knobs::testLLKnob.readGlobal();
    ASSERT_EQ(std::get<long long>(val), 9999LL);
    testAtomicLongLong.store(100LL);  // Restore.
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesSynchronizedEnum) {
    *testSyncEnum = QueryFrameworkControlEnum::kForceClassicEngine;
    auto val = test_knobs::testEnumKnob.readGlobal();
    ASSERT_EQ(std::get<int>(val), static_cast<int>(QueryFrameworkControlEnum::kForceClassicEngine));
    *testSyncEnum = QueryFrameworkControlEnum::kTrySbeEngine;  // Restore.
}

TEST(QueryKnobTest, EnumKnobFromBSONRoundTrip) {
    auto enumAsInt = static_cast<int>(QueryFrameworkControlEnum::kTrySbeRestricted);
    QueryKnobValue original{enumAsInt};
    BSONObjBuilder b;
    test_knobs::testEnumKnob.toBSON(b, "v"_sd, original);
    auto obj = b.obj();
    ASSERT_EQ(obj.firstElement().type(), BSONType::string);
    auto roundTripped = test_knobs::testEnumKnob.fromBSON(obj.firstElement());
    ASSERT_EQ(std::get<int>(roundTripped), enumAsInt);
}

TEST(QueryKnobTest, EnumKnobFromBSONString) {
    auto val = test_knobs::testEnumKnob.fromBSON(BSON("v" << "forceClassicEngine").firstElement());
    ASSERT_EQ(std::get<int>(val), static_cast<int>(QueryFrameworkControlEnum::kForceClassicEngine));
}

TEST(QueryKnobTest, FromBSONInt) {
    auto val = test_knobs::testIntKnob.fromBSON(BSON("x" << 99).firstElement());
    ASSERT_EQ(std::get<int>(val), 99);
}

TEST(QueryKnobTest, FromBSONDouble) {
    auto val = test_knobs::testDoubleKnob.fromBSON(BSON("x" << 1.5).firstElement());
    ASSERT_APPROX_EQUAL(std::get<double>(val), 1.5, 1e-9);
}

TEST(QueryKnobTest, FromBSONBool) {
    auto val = test_knobs::testBoolKnob.fromBSON(BSON("x" << true).firstElement());
    ASSERT_EQ(std::get<bool>(val), true);
}

TEST(QueryKnobTest, FromBSONLongLong) {
    auto val = test_knobs::testLLKnob.fromBSON(BSON("x" << 50LL).firstElement());
    ASSERT_EQ(std::get<long long>(val), 50LL);
}

TEST(QueryKnobTest, ToBSONRoundTripInt) {
    QueryKnobValue original{42};
    BSONObjBuilder b;
    test_knobs::testIntKnob.toBSON(b, "v"_sd, original);
    auto roundTripped = test_knobs::testIntKnob.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<int>(roundTripped), 42);
}

TEST(QueryKnobTest, ToBSONRoundTripDouble) {
    QueryKnobValue original{2.718};
    BSONObjBuilder b;
    test_knobs::testDoubleKnob.toBSON(b, "v"_sd, original);
    auto roundTripped = test_knobs::testDoubleKnob.fromBSON(b.obj().firstElement());
    ASSERT_APPROX_EQUAL(std::get<double>(roundTripped), 2.718, 1e-9);
}

TEST(QueryKnobTest, ToBSONRoundTripBool) {
    QueryKnobValue original{false};
    BSONObjBuilder b;
    test_knobs::testBoolKnob.toBSON(b, "v"_sd, original);
    auto roundTripped = test_knobs::testBoolKnob.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<bool>(roundTripped), false);
}

TEST(QueryKnobTest, ToBSONRoundTripLongLong) {
    QueryKnobValue original{777LL};
    BSONObjBuilder b;
    test_knobs::testLLKnob.toBSON(b, "v"_sd, original);
    auto roundTripped = test_knobs::testLLKnob.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<long long>(roundTripped), 777LL);
}

TEST(QueryKnobTest, MonostateDefault) {
    QueryKnobValue v;
    ASSERT(std::holds_alternative<std::monostate>(v));
}

TEST(QueryKnobTest, ParamNameStoredCorrectly) {
    ASSERT_EQ(test_knobs::testIntKnob.paramName, "testIntKnob"_sd);
    ASSERT_EQ(test_knobs::testDoubleKnob.paramName, "testDoubleKnob"_sd);
    ASSERT_EQ(test_knobs::testBoolKnob.paramName, "testBoolKnob"_sd);
    ASSERT_EQ(test_knobs::testLLKnob.paramName, "testLLKnob"_sd);
    ASSERT_EQ(test_knobs::testEnumKnob.paramName, "testEnumKnob"_sd);
}

}  // namespace
}  // namespace mongo
