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

#include "mongo/db/query/query_knobs/query_knob.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/db/query/query_knobs/query_knob_test_gen.h"
#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

template <typename T>
const QueryKnobRegistry::Entry& entryFor(const QueryKnob<T>& knob) {
    return QueryKnobRegistry::instance().entry(knob.id);
}

TEST(QueryKnobTest, SyntheticKnobsRegisteredAgainstServerParameters) {
    ASSERT_EQ(entryFor(test_knobs::testIntKnob).param->name(), "testIntKnob"_sd);
    ASSERT_EQ(entryFor(test_knobs::testDoubleKnob).param->name(), "testDoubleKnob"_sd);
    ASSERT_EQ(entryFor(test_knobs::testBoolKnob).param->name(), "testBoolKnob"_sd);
    ASSERT_EQ(entryFor(test_knobs::testLLKnob).param->name(), "testLLKnob"_sd);
    ASSERT_EQ(entryFor(test_knobs::testEnumKnob).param->name(), "testEnumKnob"_sd);
}

TEST(QueryKnobTest, ReadGlobalInt) {
    auto val = entryFor(test_knobs::testIntKnob).readGlobal();
    ASSERT(std::holds_alternative<int>(val));
    ASSERT_EQ(std::get<int>(val), 42);
}

TEST(QueryKnobTest, ReadGlobalDouble) {
    auto val = entryFor(test_knobs::testDoubleKnob).readGlobal();
    ASSERT(std::holds_alternative<double>(val));
    ASSERT_APPROX_EQUAL(std::get<double>(val), 3.14, 1e-9);
}

TEST(QueryKnobTest, ReadGlobalBool) {
    auto val = entryFor(test_knobs::testBoolKnob).readGlobal();
    ASSERT(std::holds_alternative<bool>(val));
    ASSERT_EQ(std::get<bool>(val), true);
}

TEST(QueryKnobTest, ReadGlobalLongLong) {
    auto val = entryFor(test_knobs::testLLKnob).readGlobal();
    ASSERT(std::holds_alternative<long long>(val));
    ASSERT_EQ(std::get<long long>(val), 100LL);
}

TEST(QueryKnobTest, ReadGlobalSynchronizedEnum) {
    auto val = entryFor(test_knobs::testEnumKnob).readGlobal();
    ASSERT(std::holds_alternative<int>(val));
    ASSERT_EQ(std::get<int>(val), static_cast<int>(TestKnobModeEnum::kAlpha));
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesInt) {
    RAIIServerParameterControllerForTest controller("testIntKnob", 999);
    auto val = entryFor(test_knobs::testIntKnob).readGlobal();
    ASSERT_EQ(std::get<int>(val), 999);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesDouble) {
    RAIIServerParameterControllerForTest controller("testDoubleKnob", 6.28);
    auto val = entryFor(test_knobs::testDoubleKnob).readGlobal();
    ASSERT_APPROX_EQUAL(std::get<double>(val), 6.28, 1e-9);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesBool) {
    RAIIServerParameterControllerForTest controller("testBoolKnob", false);
    auto val = entryFor(test_knobs::testBoolKnob).readGlobal();
    ASSERT_EQ(std::get<bool>(val), false);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesLongLong) {
    RAIIServerParameterControllerForTest controller("testLLKnob", 9999LL);
    auto val = entryFor(test_knobs::testLLKnob).readGlobal();
    ASSERT_EQ(std::get<long long>(val), 9999LL);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesSynchronizedEnum) {
    RAIIServerParameterControllerForTest controller("testEnumKnob", "beta");
    auto val = entryFor(test_knobs::testEnumKnob).readGlobal();
    ASSERT_EQ(std::get<int>(val), static_cast<int>(TestKnobModeEnum::kBeta));
}

TEST(QueryKnobTest, EnumKnobFromBSONRoundTrip) {
    const auto& e = entryFor(test_knobs::testEnumKnob);
    auto enumAsInt = static_cast<int>(TestKnobModeEnum::kBeta);
    QueryKnobValue original{enumAsInt};
    BSONObjBuilder b;
    e.toBSON(b, "v"_sd, original);
    auto obj = b.obj();
    ASSERT_EQ(obj.firstElement().type(), BSONType::string);
    auto roundTripped = e.fromBSON(obj.firstElement());
    ASSERT_EQ(std::get<int>(roundTripped), enumAsInt);
}

TEST(QueryKnobTest, EnumKnobFromBSONString) {
    auto val = entryFor(test_knobs::testEnumKnob).fromBSON(BSON("v" << "beta").firstElement());
    ASSERT_EQ(std::get<int>(val), static_cast<int>(TestKnobModeEnum::kBeta));
}

TEST(QueryKnobTest, FromBSONInt) {
    auto val = entryFor(test_knobs::testIntKnob).fromBSON(BSON("x" << 99).firstElement());
    ASSERT_EQ(std::get<int>(val), 99);
}

TEST(QueryKnobTest, FromBSONDouble) {
    auto val = entryFor(test_knobs::testDoubleKnob).fromBSON(BSON("x" << 1.5).firstElement());
    ASSERT_APPROX_EQUAL(std::get<double>(val), 1.5, 1e-9);
}

TEST(QueryKnobTest, FromBSONBool) {
    auto val = entryFor(test_knobs::testBoolKnob).fromBSON(BSON("x" << true).firstElement());
    ASSERT_EQ(std::get<bool>(val), true);
}

TEST(QueryKnobTest, FromBSONLongLong) {
    auto val = entryFor(test_knobs::testLLKnob).fromBSON(BSON("x" << 50LL).firstElement());
    ASSERT_EQ(std::get<long long>(val), 50LL);
}

TEST(QueryKnobTest, ToBSONRoundTripInt) {
    const auto& e = entryFor(test_knobs::testIntKnob);
    QueryKnobValue original{42};
    BSONObjBuilder b;
    e.toBSON(b, "v"_sd, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<int>(roundTripped), 42);
}

TEST(QueryKnobTest, ToBSONRoundTripDouble) {
    const auto& e = entryFor(test_knobs::testDoubleKnob);
    QueryKnobValue original{2.718};
    BSONObjBuilder b;
    e.toBSON(b, "v"_sd, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_APPROX_EQUAL(std::get<double>(roundTripped), 2.718, 1e-9);
}

TEST(QueryKnobTest, ToBSONRoundTripBool) {
    const auto& e = entryFor(test_knobs::testBoolKnob);
    QueryKnobValue original{false};
    BSONObjBuilder b;
    e.toBSON(b, "v"_sd, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<bool>(roundTripped), false);
}

TEST(QueryKnobTest, ToBSONRoundTripLongLong) {
    const auto& e = entryFor(test_knobs::testLLKnob);
    QueryKnobValue original{777LL};
    BSONObjBuilder b;
    e.toBSON(b, "v"_sd, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<long long>(roundTripped), 777LL);
}

TEST(QueryKnobTest, DeleteQueryKnobOverrideDefault) {
    QueryKnobValue v;
    ASSERT(std::holds_alternative<DeleteQueryKnobOverride>(v));
}

}  // namespace
}  // namespace mongo
