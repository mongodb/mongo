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

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

namespace mongo {
namespace {

using InternalUnpackBucketGenerateInPipelineTest = AggregationContextFixture;

// Helper datatype to make it easier to write tests by specifying only the arguments of interest.
struct RewritePipelineHelperArgs {
    const StringData timeField = "time"_sd;
    const boost::optional<StringData> metaField = boost::none;
    const boost::optional<std::int32_t> bucketMaxSpanSeconds = boost::none;
    const mongo::OptionalBool timeseriesBucketsMayHaveMixedSchemaData = {boost::none};
    const bool timeseriesBucketsAreFixed = {false};
};

// Helper function to call the factory function and ensure some basic expected truths.
std::tuple<std::vector<BSONObj>, BSONObj> rewritePipelineHelper(
    const RewritePipelineHelperArgs& args = {},
    const std::vector<BSONObj>& originalPipeline = std::vector{BSON("$match" << BSON("a" << 1))}) {
    const auto alteredPipeline = DocumentSourceInternalUnpackBucket::generateStageInPipeline(
        originalPipeline,
        args.timeField,
        args.metaField,
        args.bucketMaxSpanSeconds,
        args.timeseriesBucketsMayHaveMixedSchemaData,
        args.timeseriesBucketsAreFixed);

    ASSERT_EQ(alteredPipeline.size(), originalPipeline.size() + 1);

    // The first stage should be the generated $_internalUnpackBucket stage.
    const auto firstStage = *alteredPipeline.begin();
    ASSERT_EQ(firstStage.firstElementFieldName(),
              DocumentSourceInternalUnpackBucket::kStageNameInternal);

    return {alteredPipeline,
            firstStage[DocumentSourceInternalUnpackBucket::kStageNameInternal].Obj()};
}

TEST_F(InternalUnpackBucketGenerateInPipelineTest, EnsureStageIsGeneratedInReturnedPipeline) {
    const auto originalPipeline = std::vector{BSON("$match" << BSON("a" << 1))};
    const auto [alteredPipeline, _] = rewritePipelineHelper({}, originalPipeline);

    // The rest of the stages should be unchanged.
    for (auto oitr = originalPipeline.begin(), aitr = alteredPipeline.begin() + 1;
         oitr != originalPipeline.end() && aitr != alteredPipeline.end();
         ++oitr, ++aitr) {
        ASSERT_BSONOBJ_EQ(*aitr, *oitr);
    }
}

TEST_F(InternalUnpackBucketGenerateInPipelineTest, ValidateFields) {
    {
        const auto [alteredPipeline, _] = rewritePipelineHelper();
        // The first stage should be the generated $_internalUnpackBucket stage.
        const auto& firstStage = *alteredPipeline.begin();
        ASSERT_BSONOBJ_EQ(
            BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                 << BSON(DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData
                         << false << DocumentSourceInternalUnpackBucket::kFixedBuckets << false)),
            firstStage);
    }

    {
        const auto [alteredPipeline, _] = rewritePipelineHelper({
            .timeseriesBucketsMayHaveMixedSchemaData = false,
            .timeseriesBucketsAreFixed = true,
        });
        // The first stage should be the generated $_internalUnpackBucket stage.
        const auto& firstStage = *alteredPipeline.begin();
        ASSERT_BSONOBJ_EQ(
            BSON(DocumentSourceInternalUnpackBucket::kStageNameInternal
                 << BSON(DocumentSourceInternalUnpackBucket::kAssumeNoMixedSchemaData
                         << true << DocumentSourceInternalUnpackBucket::kFixedBuckets << true)),
            firstStage);
    }
}

TEST_F(InternalUnpackBucketGenerateInPipelineTest, BucketsFixedTest) {
    {
        const auto [_, internalUnpackBucketStage] =
            rewritePipelineHelper({.timeseriesBucketsAreFixed = false});
        ASSERT_FALSE(
            internalUnpackBucketStage[DocumentSourceInternalUnpackBucket::kFixedBuckets].Bool());
    }

    {
        const auto [_, internalUnpackBucketStage] =
            rewritePipelineHelper({.timeseriesBucketsAreFixed = true});
        ASSERT_TRUE(
            internalUnpackBucketStage[DocumentSourceInternalUnpackBucket::kFixedBuckets].Bool());
    }
}
}  // namespace
}  // namespace mongo
