// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/db/query/query_knobs/query_knob_test_gen.h"
#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/server_parameter_guard.h"

#include <algorithm>
#include <string_view>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;

template <typename T>
const QueryKnobRegistry::Entry& entryFor(const QueryKnob<T>& knob) {
    return QueryKnobRegistry::instance().entry(knob.id);
}

// Exercises the generated AccessorMixin getters: each getter must dispatch to the matching
// get() overload, passing the QueryKnob handle declared for that row in the EXPAND table.
struct MockKnobConfiguration : test_knobs::AccessorMixinTestKnobs<MockKnobConfiguration> {
    int get(const QueryKnob<int>& knob) const {
        ASSERT_EQ(&knob, &test_knobs::testIntKnob);
        return 7;
    }
    double get(const QueryKnob<double>& knob) const {
        ASSERT_EQ(&knob, &test_knobs::testDoubleKnob);
        return 1.5;
    }
    bool get(const QueryKnob<bool>& knob) const {
        ASSERT_EQ(&knob, &test_knobs::testBoolKnob);
        return true;
    }
    long long get(const QueryKnob<long long>& knob) const {
        ASSERT_EQ(&knob, &test_knobs::testLLKnob);
        return 42LL;
    }
    TestKnobModeEnum get(const QueryKnob<TestKnobModeEnum>& knob) const {
        ASSERT_EQ(&knob, &test_knobs::testEnumKnob);
        return TestKnobModeEnum::kBeta;
    }
};

TEST(QueryKnobTest, SyntheticKnobsRegisteredAgainstServerParameters) {
    ASSERT_EQ(entryFor(test_knobs::testIntKnob).param->name(), "testIntKnob"sv);
    ASSERT_EQ(entryFor(test_knobs::testDoubleKnob).param->name(), "testDoubleKnob"sv);
    ASSERT_EQ(entryFor(test_knobs::testBoolKnob).param->name(), "testBoolKnob"sv);
    ASSERT_EQ(entryFor(test_knobs::testLLKnob).param->name(), "testLLKnob"sv);
    ASSERT_EQ(entryFor(test_knobs::testEnumKnob).param->name(), "testEnumKnob"sv);
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
    unittest::ServerParameterGuard controller("testIntKnob", 999);
    auto val = entryFor(test_knobs::testIntKnob).readGlobal();
    ASSERT_EQ(std::get<int>(val), 999);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesDouble) {
    unittest::ServerParameterGuard controller("testDoubleKnob", 6.28);
    auto val = entryFor(test_knobs::testDoubleKnob).readGlobal();
    ASSERT_APPROX_EQUAL(std::get<double>(val), 6.28, 1e-9);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesBool) {
    unittest::ServerParameterGuard controller("testBoolKnob", false);
    auto val = entryFor(test_knobs::testBoolKnob).readGlobal();
    ASSERT_EQ(std::get<bool>(val), false);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesLongLong) {
    unittest::ServerParameterGuard controller("testLLKnob", 9999LL);
    auto val = entryFor(test_knobs::testLLKnob).readGlobal();
    ASSERT_EQ(std::get<long long>(val), 9999LL);
}

TEST(QueryKnobTest, ReadGlobalReflectsUpdatesSynchronizedEnum) {
    unittest::ServerParameterGuard controller("testEnumKnob", "beta");
    auto val = entryFor(test_knobs::testEnumKnob).readGlobal();
    ASSERT_EQ(std::get<int>(val), static_cast<int>(TestKnobModeEnum::kBeta));
}

TEST(QueryKnobTest, EnumKnobFromBSONRoundTrip) {
    const auto& e = entryFor(test_knobs::testEnumKnob);
    auto enumAsInt = static_cast<int>(TestKnobModeEnum::kBeta);
    QueryKnobValue original{enumAsInt};
    BSONObjBuilder b;
    e.toBSON(b, "v"sv, original);
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
    e.toBSON(b, "v"sv, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<int>(roundTripped), 42);
}

TEST(QueryKnobTest, ToBSONRoundTripDouble) {
    const auto& e = entryFor(test_knobs::testDoubleKnob);
    QueryKnobValue original{2.718};
    BSONObjBuilder b;
    e.toBSON(b, "v"sv, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_APPROX_EQUAL(std::get<double>(roundTripped), 2.718, 1e-9);
}

TEST(QueryKnobTest, ToBSONRoundTripBool) {
    const auto& e = entryFor(test_knobs::testBoolKnob);
    QueryKnobValue original{false};
    BSONObjBuilder b;
    e.toBSON(b, "v"sv, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<bool>(roundTripped), false);
}

TEST(QueryKnobTest, ToBSONRoundTripLongLong) {
    const auto& e = entryFor(test_knobs::testLLKnob);
    QueryKnobValue original{777LL};
    BSONObjBuilder b;
    e.toBSON(b, "v"sv, original);
    auto roundTripped = e.fromBSON(b.obj().firstElement());
    ASSERT_EQ(std::get<long long>(roundTripped), 777LL);
}

TEST(QueryKnobTest, DeleteQueryKnobOverrideDefault) {
    QueryKnobValue v;
    ASSERT(std::holds_alternative<DeleteQueryKnobOverride>(v));
}

TEST(QueryKnobTest, DefineQueryKnobGettersDispatchesToGet) {
    MockKnobConfiguration mock;
    ASSERT_EQ(mock.getTestInt(), 7);
    ASSERT_APPROX_EQUAL(mock.getTestDouble(), 1.5, 1e-9);
    ASSERT_EQ(mock.getTestBool(), true);
    ASSERT_EQ(mock.getTestLL(), 42LL);
    ASSERT_EQ(mock.getTestEnum(), TestKnobModeEnum::kBeta);
}

}  // namespace
}  // namespace mongo
