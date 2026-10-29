// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
