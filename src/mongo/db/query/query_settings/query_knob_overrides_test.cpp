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

#include "mongo/db/query/query_settings/query_knob_overrides.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knob.h"
#include "mongo/db/query/query_settings/query_knob_overrides_test_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

#include <string>

namespace mongo::query_settings {

// QueryKnob<T> descriptors bound to the ServerParameters in query_knob_overrides_test.idl.
// TODO SERVER-125993: Cover enum knob.
namespace test_knobs {
inline QueryKnob<int> intKnob{"overridesTestInt", &readGlobalValue<gOverridesTestInt>};
inline QueryKnob<bool> boolKnob{"overridesTestBool", &readGlobalValue<gOverridesTestBool>};
}  // namespace test_knobs

namespace {

TEST(QuerySettingsKnobOverridesTest, EmptyBSONYieldsEmptyOverrides) {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSONObj{});
    ASSERT_TRUE(overrides.empty());
    ASSERT_EQ(overrides.entries().size(), 0u);
}

TEST(QuerySettingsKnobOverridesTest, RoundTripInt) {
    auto bson = BSON("overridesTestIntWire" << 99);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_FALSE(overrides.empty());
    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<int>(overrides.entries()[0].value));
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 99);
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, RoundTripBool) {
    auto bson = BSON("overridesTestBoolWire" << true);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<bool>(overrides.entries()[0].value));
    ASSERT_TRUE(std::get<bool>(overrides.entries()[0].value));
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, RoundTripMultipleKnobs) {
    auto bson = BSON("overridesTestIntWire" << 7 << "overridesTestBoolWire" << false);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 2u);
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, NullElementStoresDeleteQueryKnobOverride) {
    auto bson = BSON("overridesTestIntWire" << BSONNULL);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 1u);
    ASSERT_TRUE(std::holds_alternative<DeleteQueryKnobOverride>(overrides.entries()[0].value));
}

TEST(QuerySettingsKnobOverridesTest, DuplicateKnob) {
    auto bson = BSON("overridesTestIntWire" << 5 << "overridesTestIntWire" << 10);
    auto overrides = QuerySettingsKnobOverrides::fromBSON(bson);

    ASSERT_EQ(overrides.entries().size(), 2u);
    ASSERT_EQ(std::get<int>(overrides.entries()[0].value), 5);
    ASSERT_EQ(std::get<int>(overrides.entries()[1].value), 10);
    ASSERT_BSONOBJ_EQ(overrides.toBSON(), bson);
}

TEST(QuerySettingsKnobOverridesTest, WrongTypeThrows) {
    ASSERT_THROWS_CODE(
        QuerySettingsKnobOverrides::fromBSON(BSON("overridesTestIntWire" << "notAnInt")),
        DBException,
        12194501);
}

TEST(QuerySettingsKnobOverridesTest, ValidKnobBeforeInvalidKnobThrows) {
    auto bson = BSON("overridesTestIntWire" << 5 << "overridesTestBoolWire"
                                            << "notABool");
    ASSERT_THROWS_CODE(QuerySettingsKnobOverrides::fromBSON(bson), DBException, 12194501);
}

TEST(QuerySettingsKnobOverridesTest, UnknownKnobThrows) {
    ASSERT_THROWS_CODE(QuerySettingsKnobOverrides::fromBSON(BSON("totallyUnknownKnob" << 1)),
                       DBException,
                       12194500);
}

DEATH_TEST_REGEX(QuerySettingsKnobOverridesDeathTest,
                 ToBSONWithDeleteQueryKnobOverrideTasserts,
                 "12194502.*DeleteQueryKnobOverride must not survive past simplification") {
    auto overrides = QuerySettingsKnobOverrides::fromBSON(BSON("overridesTestIntWire" << BSONNULL));
    overrides.toBSON();
}

}  // namespace
}  // namespace mongo::query_settings
