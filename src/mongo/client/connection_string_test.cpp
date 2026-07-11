// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/connection_string.h"

#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"

#include <fmt/format.h>

namespace mongo {
namespace {

using unittest::assertGet;

TEST(ConnectionString, EqualityOperatorStandalone) {
    const auto cs = assertGet(ConnectionString::parse("TestHostA:12345"));
    ASSERT(cs == assertGet(ConnectionString::parse("TestHostA:12345")));
    ASSERT_FALSE(cs != assertGet(ConnectionString::parse("TestHostA:12345")));
    ASSERT(cs != assertGet(ConnectionString::parse("TestHostB:12345")));
    ASSERT_FALSE(cs == assertGet(ConnectionString::parse("TestHostB:12345")));
}

TEST(ConnectionString, EqualityOperatorReplicaSet) {
    const auto cs = assertGet(ConnectionString::parse("TestRS/TestHostA:12345,TestHostB:12345"));
    ASSERT(cs == assertGet(ConnectionString::parse("TestRS/TestHostA:12345,TestHostB:12345")));
    ASSERT_FALSE(cs == assertGet(ConnectionString::parse("TestHostA:12345")));
    ASSERT_FALSE(cs ==
                 assertGet(ConnectionString::parse("TestRS/TestHostB:12345,TestHostA:12345")));
    ASSERT_FALSE(cs ==
                 assertGet(ConnectionString::parse("TestRS1/TestHostA:12345,TestHostB:12345")));
}

}  // namespace
}  // namespace mongo
