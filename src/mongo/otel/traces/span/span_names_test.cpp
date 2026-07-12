// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/traces/span/span_names.h"

#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::traces {
namespace {

TEST(SpanNamesTest, RegisterAndLookupCommandSpanNames) {
    const auto& firstSpan = registerCommandSpanName("test_only.command1");
    const auto& secondSpan = registerCommandSpanName("test_only.command2");
    const auto& thirdSpan = registerCommandSpanName("test_only.command3");

    EXPECT_EQ(firstSpan.getName(), "test_only.command1");
    EXPECT_EQ(secondSpan.getName(), "test_only.command2");
    EXPECT_EQ(thirdSpan.getName(), "test_only.command3");

    auto* foundFirst = lookupCommandSpanName("test_only.command1");
    EXPECT_TRUE(foundFirst != nullptr);
    EXPECT_EQ(foundFirst, &firstSpan);

    auto* foundSecond = lookupCommandSpanName("test_only.command2");
    EXPECT_TRUE(foundSecond != nullptr);
    EXPECT_EQ(foundSecond, &secondSpan);

    auto* foundThird = lookupCommandSpanName("test_only.command3");
    EXPECT_TRUE(foundThird != nullptr);
    EXPECT_EQ(foundThird, &thirdSpan);
}

TEST(SpanNamesTest, LookupUnregisteredNameReturnsNullptr) {
    EXPECT_TRUE(lookupCommandSpanName("test_only.never_registered_command") == nullptr);
}

TEST(SpanNamesTest, DuplicateRegistrationReturnsExistingSpanName) {
    const auto& first = registerCommandSpanName("test_only.duplicate_command");
    const auto& second = registerCommandSpanName("test_only.duplicate_command");

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(first.getName(), "test_only.duplicate_command");

    auto* found = lookupCommandSpanName("test_only.duplicate_command");
    EXPECT_EQ(found, &first);
}

}  // namespace
}  // namespace mongo::otel::traces
