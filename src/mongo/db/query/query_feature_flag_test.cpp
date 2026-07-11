// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/feature_flag.h"
#include "mongo/db/feature_flag_test_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using namespace testing;

MATCHER_P(Name, m, "") {
    return ExplainMatchResult(m, arg.getName(), result_listener);
}

MATCHER_P(ShouldSerializeOnOutgoingRequests, m, "") {
    return ExplainMatchResult(m, arg.shouldSerializeOnOutgoingRequests(), result_listener);
}

auto outFlags() {
    return IncrementalRolloutFeatureFlag::getFlagsIntroducedSinceLastLTS();
}

TEST(IDLFeatureFlag, GetFlagsForOutgoingRequests) {
    ASSERT_THAT(outFlags(), Each(Pointee(ShouldSerializeOnOutgoingRequests(Eq(true)))));
}

TEST(IDLFeatureFlag, SearchExtensionFlagSerializedToShards) {
    ASSERT_THAT(outFlags(),
                Contains(Pointee(Name(Eq(feature_flags::kFeatureFlagSearchExtensionName)))));
}

TEST(IDLFeatureFlag, VectorSearchExtensionFlagSerializedToShards) {
    ASSERT_THAT(outFlags(),
                Contains(Pointee(Name(Eq(feature_flags::kFeatureFlagVectorSearchExtensionName)))));
}

}  // namespace
}  // namespace mongo
