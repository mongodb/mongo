// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/span/span_names.h"

#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::traces {
namespace {

TEST(SpanNamesTest, RegisterCommandSpanNames) {
    const auto& firstSpan = registerCommandSpanName("test_only.command1");
    const auto& secondSpan = registerCommandSpanName("test_only.command2");
    const auto& thirdSpan = registerCommandSpanName("test_only.command3");

    EXPECT_EQ(firstSpan.getName(), "test_only.command1");
    EXPECT_EQ(secondSpan.getName(), "test_only.command2");
    EXPECT_EQ(thirdSpan.getName(), "test_only.command3");

    EXPECT_EQ(&getOrRegisterCommandSpanName("test_only.command1"), &firstSpan);
    EXPECT_EQ(&getOrRegisterCommandSpanName("test_only.command2"), &secondSpan);
    EXPECT_EQ(&getOrRegisterCommandSpanName("test_only.command3"), &thirdSpan);
}

TEST(SpanNamesTest, DuplicateRegistrationReturnsExistingSpanName) {
    const auto& first = registerCommandSpanName("test_only.duplicate_command");
    const auto& second = registerCommandSpanName("test_only.duplicate_command");

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(first.getName(), "test_only.duplicate_command");
    EXPECT_EQ(&getOrRegisterCommandSpanName("test_only.duplicate_command"), &first);
}

TEST(SpanNamesTest, GetOrRegisterCommandSpanNameRegistersOnFirstUse) {
    const auto& firstLookup = getOrRegisterCommandSpanName("test_only.get_or_register");
    EXPECT_EQ(firstLookup.getName(), "test_only.get_or_register");

    const auto& secondLookup = getOrRegisterCommandSpanName("test_only.get_or_register");
    EXPECT_EQ(&firstLookup, &secondLookup);
}

TEST(SpanNamesTest, GetOrRegisterCommandSpanNameReturnsExistingRegistration) {
    const auto& registered = registerCommandSpanName("test_only.get_or_register_existing");
    const auto& lookedUp = getOrRegisterCommandSpanName("test_only.get_or_register_existing");

    EXPECT_EQ(&registered, &lookedUp);
}

TEST(SpanNamesTest, GetOrRegisterCommandSpanNameReturnsMongoRpcForEmptyName) {
    EXPECT_EQ(&getOrRegisterCommandSpanName(""), &span_names::kMongoRPC);
}

DEATH_TEST(SpanNamesDeathTest, RegisterCommandSpanNameRejectsEmptyName, "invariant") {
    registerCommandSpanName("");
}

}  // namespace
}  // namespace mongo::otel::traces
