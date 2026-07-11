// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/serialization_options.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(SerializationOptionsTest, IsDefaultSerialization) {
    query_shape::SerializationOptions opts;

    ASSERT_TRUE(opts.isDefaultSerialization());
    ASSERT_TRUE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_FALSE(opts.isSerializingForQueryStats());
}

TEST(SerializationOptionsTest, DebugTypeStringPolicy) {
    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToDebugTypeString;

    ASSERT_FALSE(opts.isKeepingLiteralsUnchanged());
    ASSERT_TRUE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_TRUE(opts.isSerializingForQueryStats());  // Since policy is NOT unchanged
}

TEST(SerializationOptionsTest, RepresentativeParseableValuePolicy) {
    query_shape::SerializationOptions opts;
    opts.literalPolicy = query_shape::LiteralSerializationPolicy::kToRepresentativeParseableValue;

    ASSERT_FALSE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_TRUE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_TRUE(opts.isSerializingForQueryStats());  // Since policy is NOT unchanged
}

TEST(SerializationOptionsTest, ExplainModeEnabled) {
    query_shape::SerializationOptions opts;
    opts.verbosity = ExplainOptions::Verbosity::kQueryPlanner;

    ASSERT_TRUE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_TRUE(opts.isSerializingForExplain());
    ASSERT_FALSE(opts.isSerializingForQueryStats());  // QueryStats is unrelated to Explain mode
}

TEST(SerializationOptionsTest, TransformIdentifiersEnabled) {
    query_shape::SerializationOptions opts;
    opts.transformIdentifiers = true;

    ASSERT_TRUE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_TRUE(opts.isSerializingForQueryStats());  // Since transformIdentifiers is enabled
}

}  // namespace
}  // namespace mongo
