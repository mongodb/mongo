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

#include "mongo/otel/traces/span/span_names.h"

#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/sampler/sampling_config.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel::traces {
namespace {

TEST(SpanNamesTest, RegisterAndLookupCommandSpanNames) {
    const auto& firstSpan = registerCommandSpanName("test_only.command1", SampledByDefault{false});
    const auto& secondSpan = registerCommandSpanName("test_only.command2", SampledByDefault{true});
    const auto& thirdSpan = registerCommandSpanName("test_only.command3", SampledByDefault{false});

    EXPECT_EQ(firstSpan.getName(), "test_only.command1");
    EXPECT_EQ(static_cast<bool>(firstSpan.getSampledByDefault()), false);
    EXPECT_EQ(secondSpan.getName(), "test_only.command2");
    EXPECT_EQ(static_cast<bool>(secondSpan.getSampledByDefault()), true);
    EXPECT_EQ(thirdSpan.getName(), "test_only.command3");
    EXPECT_EQ(static_cast<bool>(thirdSpan.getSampledByDefault()), false);

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
    const auto& first =
        registerCommandSpanName("test_only.duplicate_command", SampledByDefault{false});
    const auto& second =
        registerCommandSpanName("test_only.duplicate_command", SampledByDefault{false});

    EXPECT_EQ(&first, &second);
    EXPECT_EQ(first.getName(), "test_only.duplicate_command");
    EXPECT_EQ(static_cast<bool>(first.getSampledByDefault()), false);

    auto* found = lookupCommandSpanName("test_only.duplicate_command");
    EXPECT_EQ(found, &first);
}

}  // namespace
}  // namespace mongo::otel::traces
