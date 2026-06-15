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
#include "mongo/db/query/query_knobs/query_knob_change_notifier.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/db/query/query_knobs/query_knob_test_gen.h"
#include "mongo/db/query/query_knobs/query_knob_test_knobs.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/server_parameter_guard.h"

#include <utility>
#include <vector>

namespace mongo {
namespace {

// Process-wide event recorder driven by the listener registered below. Each
// test drains at entry so events from earlier cases (including the restore
// fired by unittest::ServerParameterGuard's destructor) do not leak in.
std::vector<QueryKnobChange>& recordedEvents() {
    static std::vector<QueryKnobChange> v;
    return v;
}

std::vector<QueryKnobChange> drainEvents() {
    return std::exchange(recordedEvents(), {});
}

}  // namespace

REGISTER_QUERY_KNOBS_LISTENER(QueryKnobListenerTestRecorder, ([](const QueryKnobChange& e) {
                                  recordedEvents().push_back({e.id, e.newValue});
                                  return Status::OK();
                              }))

namespace {

TEST(QueryKnobListenerTest, AtomicIntKnobFiresListenerWithIntPayload) {
    drainEvents();
    unittest::ServerParameterGuard scoped{"testIntKnob", 7};

    auto events = drainEvents();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].id, test_knobs::testIntKnob.id);
    ASSERT_EQ(std::get<int>(events[0].newValue), 7);
}

TEST(QueryKnobListenerTest, AtomicLongLongKnobFiresListenerWithLongLongPayload) {
    drainEvents();
    unittest::ServerParameterGuard scoped{"testLLKnob", 9999999999LL};

    auto events = drainEvents();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].id, test_knobs::testLLKnob.id);
    ASSERT_EQ(std::get<long long>(events[0].newValue), 9999999999LL);
}

TEST(QueryKnobListenerTest, AtomicDoubleKnobFiresListenerWithDoublePayload) {
    drainEvents();
    unittest::ServerParameterGuard scoped{"testDoubleKnob", 2.5};

    auto events = drainEvents();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].id, test_knobs::testDoubleKnob.id);
    ASSERT_EQ(std::get<double>(events[0].newValue), 2.5);
}

TEST(QueryKnobListenerTest, AtomicBoolKnobFiresListenerWithBoolPayload) {
    drainEvents();
    unittest::ServerParameterGuard scoped{"testBoolKnob", false};

    auto events = drainEvents();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].id, test_knobs::testBoolKnob.id);
    ASSERT_EQ(std::get<bool>(events[0].newValue), false);
}

// Enum knobs are stored as int in QueryKnobValue (see query_knob.h); the
// listener receives the integer encoding of the new enum value.
TEST(QueryKnobListenerTest, EnumKnobFiresListenerWithIntPayload) {
    drainEvents();
    unittest::ServerParameterGuard guard{"testEnumKnob", "beta"};

    auto events = drainEvents();
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0].id, test_knobs::testEnumKnob.id);
    ASSERT_EQ(std::get<int>(events[0].newValue), static_cast<int>(TestKnobModeEnum::kBeta));
}

}  // namespace
}  // namespace mongo
