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
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using DocumentSourceScoreFusionTest = AggregationContextFixture;

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNoInputsField) {
    auto spec = fromjson(R"({
        $scoreFusion: {
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfUnknownField) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            unknown: "bad field",
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInputsIsNotArray) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: "not an array",
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNoPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfMissingPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
                {
                    as: "score1"
                }
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineAllowed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
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
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelinesAllowed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
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
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfUnknownFieldInsideInputs) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    }
                ],
                unknown: "bad field"
               }
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfAsIsNotString) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: 1.5
               }
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineWithAsAllowed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
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
                ],
                as: "score1"
               }
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultipleStagesInPipelineAllowed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ]
               }
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultipleStagesInPipelineWithAsAllowed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            inputNormalization: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInputNormalizationNotString) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            inputNormalization: 10
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, CheckAnyTypeAllowedForScore) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            score: "expression",
            inputNormalization: "none"
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, ErrorIfWeightsNotArray) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: 10.8
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorIfWeightsNotArrayOfSafeDoubles) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: ["hi"]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, CheckIfWeightsArrayAllInts) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
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
                ],
                as: "score1"
               },
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    }
                ]
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: [5, 100]
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckIfWeightsArrayAllDecimals) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
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
                ],
                as: "score1"
               },
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    }
                ]
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: [5.5, 100.2]
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckIfWeightsArrayMixedIntsDecimals) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               },
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    }
                ]
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: [5, 100.2]
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckAnyTypeAllowedForScoreNulls) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: [],
            scoreNulls: 0
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, ErrorIfOptionalFieldsIncludedMoreThanOnce) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: [5],
            scoreNulls: 0,
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            inputNormalization: "none",
            weights: [5],
            scoreNulls: 0,
            score: "someExpression"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLDuplicateField);
}

TEST_F(DocumentSourceScoreFusionTest, CheckAllOptionalArgsIncluded) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ],
                as: "score1"
               }
            ],
            score: "expression",
            inputNormalization: "none",
            weights: [5],
            scoreNulls: 0
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckSomeOptionalArgsUnspecified) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: [
               {
                pipeline: [
                    {
                        $search: {
                            index: "search_index",
                            text: {
                                query: "mystery",
                                path: "genres"
                            }
                        }
                    },
                    { $match : { author : "dave" } }
                ]
               }
            ],
            inputNormalization: "none",
            weights: [5],
            scoreNulls: 0
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

}  // namespace
}  // namespace mongo
