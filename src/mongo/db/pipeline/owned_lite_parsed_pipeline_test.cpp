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
    OwnedLiteParsedPipeline owned(kNss, {});
    ASSERT_EQ(owned.pipeline().getInvolvedNamespaces().size(), 0U);
}

}  // namespace
}  // namespace mongo
