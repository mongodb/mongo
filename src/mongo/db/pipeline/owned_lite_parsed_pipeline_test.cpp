// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.coll");

// Verifies that the pipeline remains usable after the vector used to construct it is destroyed.
// With ASAN enabled, this catches actual use-after-free if ownership isn't established.
TEST(OwnedLiteParsedPipelineTest, PipelineOutlivesOriginalVector) {
    std::unique_ptr<OwnedLiteParsedPipeline> owned;
    {
        std::vector<BSONObj> stages = {BSON("$limit" << 10), BSON("$skip" << 5)};
        owned = std::make_unique<OwnedLiteParsedPipeline>(kNss, stages);
        // 'stages' is destroyed here.
    }

    // If ownership wasn't established, accessing the pipeline's stages would be a
    // use-after-free. clone() exercises the full stage-spec iteration path.
    LiteParsedPipeline cloned = owned->pipeline().clone();
    ASSERT_EQ(cloned.getInvolvedNamespaces().size(), 0U);
}

// After a move, the new owner has a valid pipeline and the BSON remains alive.
TEST(OwnedLiteParsedPipelineTest, MovedPipelineRemainsValid) {
    std::vector<BSONObj> stages = {BSON("$limit" << 10)};
    OwnedLiteParsedPipeline original(kNss, stages);

    OwnedLiteParsedPipeline moved(std::move(original));

    LiteParsedPipeline cloned = moved.pipeline().clone();
    ASSERT_EQ(cloned.getInvolvedNamespaces().size(), 0U);
}

// A copy is independently valid: destroying the original doesn't invalidate the copy.
TEST(OwnedLiteParsedPipelineTest, CopiedPipelineIsIndependent) {
    std::vector<BSONObj> stages = {BSON("$limit" << 10)};

    std::unique_ptr<OwnedLiteParsedPipeline> copy;
    {
        OwnedLiteParsedPipeline original(kNss, stages);
        copy = std::make_unique<OwnedLiteParsedPipeline>(original);
        // 'original' is destroyed here.
    }

    LiteParsedPipeline cloned = copy->pipeline().clone();
    ASSERT_EQ(cloned.getInvolvedNamespaces().size(), 0U);
}

// An empty pipeline is a valid no-op construction.
TEST(OwnedLiteParsedPipelineTest, EmptyPipelineIsValid) {
    OwnedLiteParsedPipeline owned(kNss, std::vector<BSONObj>{});
    ASSERT_EQ(owned.pipeline().getInvolvedNamespaces().size(), 0U);
}

}  // namespace
}  // namespace mongo
