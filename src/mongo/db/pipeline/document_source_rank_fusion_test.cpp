/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/json.h"

#include "mongo/unittest/assert.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {
namespace {

using DocumentSourceRankFusionTest = AggregationContextFixture;

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNoInputsField) {
    auto spec = fromjson(R"({
        $rankFusion: {
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfUnknownField) {
    auto spec = fromjson(R"({
        $rankFusion: {
            unknown: "bad field"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfInputsIsNotArray) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: "not an array"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNoPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfMissingPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
                {
                    rankConstant: 5
                }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

// TODO SERVER-92213: Remove this test or adapt it to test the stage functionality when $rankFusion
// implementation is complete.
TEST_F(DocumentSourceRankFusionTest, CheckOnePipelineAllowed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ]
               }
            ]
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

// TODO SERVER-92213: Remove this test or adapt it to test the stage functionality when $rankFusion
// implementation is complete.
TEST_F(DocumentSourceRankFusionTest, CheckMultiplePipelinesAllowed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "dave" } }
                ]
               },
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "coffee",
                                path: "flavors"
                            }
                        }
                    }
                ]
               }, 
               {
                pipeline: [
                    {
                        $vectorSearch: {
                            queryVector: [1.0, 2.0, 3.0],
                            path: "plot_embedding",
                            numCandidates: 300,
                            index: "vector_index",
                            limit: 10
                        }
                    }
                ]
               }
            ]
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfUnknownFieldInsideInputs) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "dave" } }
                ],
                unknown: "bad field"
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfRankConstantIsNotInt) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "dave" } }
                ],
                rankConstant: 1.5
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfAsIsNotString) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "dave" } }
                ],
                as: 1
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

// TODO SERVER-92213: Remove this test or adapt it to test the stage functionality when $rankFusion
// implementation is complete.
TEST_F(DocumentSourceRankFusionTest, CheckOptionalArgumentsAllowed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "dave" } }
                ],
                rankConstant: 2,
                as: "matchAuthor"
               },
               {
	            pipeline: [
                    { $match : { age : 50 } }
	            ],
                rankConstant: 5,
                as: "matchAge"
               }
            ]
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()));
}
}  // namespace
}  // namespace mongo
