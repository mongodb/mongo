/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/serialization_options.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(SerializationOptionsTest, IsDefaultSerialization) {
    SerializationOptions opts;

    ASSERT_TRUE(opts.isDefaultSerialization());
    ASSERT_TRUE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_FALSE(opts.isSerializingForQueryStats());
}

TEST(SerializationOptionsTest, DebugTypeStringPolicy) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToDebugTypeString;

    ASSERT_FALSE(opts.isKeepingLiteralsUnchanged());
    ASSERT_TRUE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_TRUE(opts.isSerializingForQueryStats());  // Since policy is NOT unchanged
}

TEST(SerializationOptionsTest, RepresentativeParseableValuePolicy) {
    SerializationOptions opts;
    opts.literalPolicy = LiteralSerializationPolicy::kToRepresentativeParseableValue;

    ASSERT_FALSE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_TRUE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_TRUE(opts.isSerializingForQueryStats());  // Since policy is NOT unchanged
}

TEST(SerializationOptionsTest, ExplainModeEnabled) {
    SerializationOptions opts;
    opts.verbosity = ExplainOptions::Verbosity::kQueryPlanner;

    ASSERT_TRUE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_TRUE(opts.isSerializingForExplain());
    ASSERT_FALSE(opts.isSerializingForQueryStats());  // QueryStats is unrelated to Explain mode
}

TEST(SerializationOptionsTest, TransformIdentifiersEnabled) {
    SerializationOptions opts;
    opts.transformIdentifiers = true;

    ASSERT_TRUE(opts.isKeepingLiteralsUnchanged());
    ASSERT_FALSE(opts.isSerializingLiteralsAsDebugTypes());
    ASSERT_FALSE(opts.isReplacingLiteralsWithRepresentativeValues());
    ASSERT_FALSE(opts.isSerializingForExplain());
    ASSERT_TRUE(opts.isSerializingForQueryStats());  // Since transformIdentifiers is enabled
}

}  // namespace
}  // namespace mongo
