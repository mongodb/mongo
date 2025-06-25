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

#include "mongo/db/pipeline/document_source_score_fusion.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

/**
 * This test fixture will provide tests with an ExpressionContext (among other things like
 * OperationContext, etc.) and configure the common feature flags that we need.
 */
class DocumentSourceScoreFusionTest : service_context_test::WithSetupTransportLayer,
                                      public AggregationContextFixture {
public:
    DocumentSourceScoreFusionTest() {
        // TODO SERVER-82020: Delete this once the feature flag defaults to true.
        // $minMaxScaler is gated behind a feature flag and does
        // not get put into the map as the flag is off by default. Changing the value of the feature
        // flag with RAIIServerParameterControllerForTest() does not solve the issue because the
        // registration logic is not re-hit.
        try {
            window_function::Expression::registerParser(
                "$minMaxScaler",
                window_function::ExpressionMinMaxScaler::parse,
                nullptr,
                AllowedWithApiStrict::kNeverInVersion1);
        } catch (const DBException& e) {
            // Allow this exception, to allow multiple instances
            // to be created in this process.
            ASSERT(e.reason() == "Duplicate parsers ($minMaxScaler) registered.");
        }
    }

private:
    RAIIServerParameterControllerForTest scoreFusionFlag{"featureFlagSearchHybridScoringFull",
                                                         true};
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest rankFusionFlag{"featureFlagRankFusionFull", true};
};

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNoInputField) {
    auto spec = fromjson(R"({
        $scoreFusion: {
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNoNestedObject) {
    auto spec = fromjson(R"({
        $scoreFusion: 'not_an_object'
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfUnknownField) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            unknown: "bad field",
            normalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInputIsNotObject) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {pipelines: "not an object", normalization: "none"}
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNoPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {},
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfMissingPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {normalization: "none"}
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfMissingNormalization) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineAllowedNormalizationNone) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineAllowedNormalizationSigmoid) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "sigmoid"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    $divide: [
                                        {
                                            $const: 1
                                        },
                                        {
                                            $add: [
                                                {
                                                    $const: 1
                                                },
                                                {
                                                    $exp: [
                                                        {
                                                            $multiply: [
                                                                {
                                                                    $const: -1
                                                                },
                                                                {
                                                                    $meta: "score"
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineAllowedNormalizationMinMaxScaler) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "minMaxScaler"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "name1_score": -1
                        },
                        "output": {
                            "name1_score": {
                                "$minMaxScaler": {
                                    "input": "$name1_score",
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelinesAllowedNormalizationMinMaxScaler) {
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        {$score: {score: "$score_50", normalization: "sigmoid"}}
                    ],
                    name2: [
                        {$score: {score: "$score_10", normalization: "sigmoid"}}
                    ]
                },
                normalization: "minMaxScaler"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$setMetadata": {
                        "score": "$score_50"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$exp": [
                                                {
                                                    "$multiply": [
                                                        {
                                                            "$const": -1
                                                        },
                                                        {
                                                            "$meta": "score"
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "name1_score": -1
                        },
                        "output": {
                            "name1_score": {
                                "$minMaxScaler": {
                                    "input": "$name1_score",
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": "$score_10"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$exp": [
                                                            {
                                                                "$multiply": [
                                                                    {
                                                                        "$const": -1
                                                                    },
                                                                    {
                                                                        "$meta": "score"
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "name2_score": -1
                                    },
                                    "output": {
                                        "name2_score": {
                                            "$minMaxScaler": {
                                                "input": "$name2_score",
                                                "min": 0,
                                                "max": 1
                                            },
                                            "window": {
                                                "documents": [
                                                    "unbounded",
                                                    "unbounded"
                                                ]
                                            }
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score",
                                "$name2_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfPipelineIsNotArray) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    authorMatch: {
                        $match : { author : "Agatha Christie" }
                    }
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfUnknownFieldInsideInput) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                unknown: "bad field",
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNotScoredPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeOne: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNotScoredPipelineWithFirstPipelineValid) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeOne: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: 5.0, normalization: "sigmoid" } }
                    ],
                    pipeTwo: [
                        { $match : { age : 50 } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNotScoredPipelineWithSecondPipelineValid) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeOne: [
                        { $match : { age : 50 } }
                    ],
                    pipeTwo: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNestedRankFusionPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $rankFusion: {
                            input: {
                                pipelines: {
                                    agatha: [
                                        { $match : { author : "Agatha Christie" } },
                                        { $sort: {author: 1} }
                                    ]
                                }
                            }
                        } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10473003);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNestedScoreFusionPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $scoreFusion: {
                            input: {
                                pipelines: {
                                    agatha: [
                                        { $match : { author : "Agatha Christie" } },
                                        { $score: {author: 1} }
                                    ]
                                },
                                normalization: "none"
                            }
                        } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10473003);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfEmptyPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeOne: []
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402503);
}

TEST_F(DocumentSourceScoreFusionTest, CheckSinglePipelineTextMatchAllowed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeOne: [
                        {
                            $match: {
                                $text: {
                                    $search: "coffee"
                                }
                            }
                        }
                    ]
                },
                normalization: "none"
            }
        }    
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelineTextMatchAllowed) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    pipeOne: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "sigmoid" } }
                    ],
                    pipeTwo: [
                        {
                            $match: {
                                $text: {
                                    $search: "coffee"
                                }
                            }
                        }
                    ]
                },
                normalization: "none"
            }
        }    
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelinesAllowed) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    name2: [
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
                },
                normalization: "none"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": { 
                        "mongotQuery": { 
                            "index": "search_index", 
                            "text": { "query": "mystery", "path": "genres" } 
                        }, 
                        "requiresSearchSequenceToken": false, 
                        "requiresSearchMetaCursor": true 
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    $meta: "score"
                                },
                                {
                                    "$const": 1.0
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                $vectorSearch: {
                                    queryVector: [1.0, 2.0, 3.0],
                                    path: "plot_embedding",
                                    numCandidates: 300,
                                    index: "vector_index",
                                    limit: 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                $meta: "score"
                                            },
                                            {
                                                "$const": 1.0
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score",
                                "$name2_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {$meta: "score"},
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelinesAllowedSigmoid) {
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        {$score: {score: "$score_50", normalization: "sigmoid"}}
                    ],
                    name2: [
                        {$score: {score: "$score_10", normalization: "sigmoid"}}
                    ]
                },
                normalization: "sigmoid"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$setMetadata": {
                        "score": "$score_50"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$exp": [
                                                {
                                                    "$multiply": [
                                                        {
                                                            "$const": -1
                                                        },
                                                        {
                                                            "$meta": "score"
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                {
                                                    "$const": 1
                                                },
                                                {
                                                    "$exp": [
                                                        {
                                                            "$multiply": [
                                                                {
                                                                    "$const": -1
                                                                },
                                                                {
                                                                    "$meta": "score"
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": "$score_10"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$exp": [
                                                            {
                                                                "$multiply": [
                                                                    {
                                                                        "$const": -1
                                                                    },
                                                                    {
                                                                        "$meta": "score"
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            {
                                                                "$const": 1
                                                            },
                                                            {
                                                                "$exp": [
                                                                    {
                                                                        "$multiply": [
                                                                            {
                                                                                "$const": -1
                                                                            },
                                                                            {
                                                                                "$meta": "score"
                                                                            }
                                                                        ]
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score",
                                "$name2_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultipleStagesInPipelineAllowed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": { 
                        "mongotQuery": { 
                            "index": "search_index", 
                            "text": { "query": "mystery", "path": "genres" } 
                        }, 
                        "requiresSearchSequenceToken": false, 
                        "requiresSearchMetaCursor": true 
                    }
                },
                {
                    $match: {
                        author: "dave"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    $meta: "score"
                                },
                                {
                                    "$const": 1.0
                                }
                            ]
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {$meta: "score"},
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelinesAndOptionalArgumentsAllowed) {
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : { $divide: [6.0, 3.0] }, normalization: "sigmoid" } }
                    ],
                    name2: [
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
                    name3: [
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
                },
                normalization: "none"
            }
        }
    })");
    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$divide": [
                                {
                                    "$const": 6
                                },
                                {
                                    "$const": 3
                                }
                            ]
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$exp": [
                                                {
                                                    "$multiply": [
                                                        {
                                                            "$const": -1
                                                        },
                                                        {
                                                            "$meta": "score"
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$search": {
                                    "mongotQuery": {
                                        "index": "search_index",
                                        "text": {
                                            "query": "mystery",
                                            "path": "genres"
                                        }
                                    },
                                    "requiresSearchSequenceToken": false,
                                    "requiresSearchMetaCursor": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$vectorSearch": {
                                    "queryVector": [
                                        1,
                                        2,
                                        3
                                    ],
                                    "path": "plot_embedding",
                                    "numCandidates": 300,
                                    "index": "vector_index",
                                    "limit": 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name3_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name3_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name3_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score",
                                "$name2_score",
                                "$name3_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNoNormalization) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNormalizationNotString) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: 1.0
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

// Note: This test errors as expected because the correct input normalization value is spelled as
// 'minMaxScaler' with an e, not 'minMaxScalar' with an a.
TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNormalizationInvalidValue) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "minMaxScalar"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfWeightsIsNotObject) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination:  {
                weights: "my bad",
                method: "avg"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, DoesNotErrorIfEmptyWeights) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                weights: {},
                method: "avg"
            }
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, DoesNotErrorIfOnlySomeWeights) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                normalization: "none"
            },
            combination: {
                weights: {
                    matchAuthor: 3
                },
                method: "avg"
            }
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfMisnamedWeight) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                normalization: "none"
            },
            combination: {
                weights: {
                    matchAuthor: 3,
                    matchGenre: 2
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9967500);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfExtraWeight) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                normalization: "none"
            },
            combination: {
                weights: {
                    matchAuthor: 3,
                    matchGenre: 2,
                    matchGenres: 1
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9460301);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNonNumericWeight) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                normalization: "none"
            },
            combination: {
                weights: {
                    matchAuthor: 3,
                    matchGenres: "0"
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       13118);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNegativeWeightValue) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                normalization: "none"
            },
            combination: {
                weights: {
                    matchAuthor: -1,
                    matchGenres: 0
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9460300);
}

TEST_F(DocumentSourceScoreFusionTest, CheckWeightsApplied) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                normalization: "none"
            },
            combination: {
                weights: {
                    matchAuthor: 5,
                    matchGenres: 3
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": {
                        "mongotQuery": {
                            "index": "search_index",
                            "text": {
                                "query": "mystery",
                                "path": "genres"
                            }
                        },
                        "requiresSearchSequenceToken": false,
                        "requiresSearchMetaCursor": true
                    }
                },
                {
                    "$match": {
                        "author": "dave"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 5
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$search": {
                                    "mongotQuery": {
                                        "index": "search_index",
                                        "text": {
                                            "query": "mystery",
                                            "path": "genres"
                                        }
                                    },
                                    "requiresSearchSequenceToken": false,
                                    "requiresSearchMetaCursor": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 3
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchGenres_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$matchAuthor_score",
                                "$matchGenres_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// Same as CheckWeightsApplied but the ordering of fields doesn't match between input.pipelines and
// combination.weights; checks that the weights are applied to the pipeline with the same name.
TEST_F(DocumentSourceScoreFusionTest, CheckWeightsAppliedToCorrectPipeline) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                normalization: "none"
            },
            combination: {
                weights: {
                    matchGenres: 3,
                    matchAuthor: 5
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": {
                        "mongotQuery": {
                            "index": "search_index",
                            "text": {
                                "query": "mystery",
                                "path": "genres"
                            }
                        },
                        "requiresSearchSequenceToken": false,
                        "requiresSearchMetaCursor": true
                    }
                },
                {
                    "$match": {
                        "author": "dave"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 5
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$search": {
                                    "mongotQuery": {
                                        "index": "search_index",
                                        "text": {
                                            "query": "mystery",
                                            "path": "genres"
                                        }
                                    },
                                    "requiresSearchSequenceToken": false,
                                    "requiresSearchMetaCursor": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 3
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchGenres_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$matchAuthor_score",
                                "$matchGenres_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckWeightsAppliedMultiplePipelines) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
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
                    matchGenres: [
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
                    matchPlot: [
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
                    matchDistance: [
                        {$geoNear: {near: [20, 40]}},
                        {$score: {score: {$meta: "geoNearDistance"}, normalization: "none"}}
                    ]
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    matchGenres: 3,
                    matchDistance: 0,
                    matchAuthor: 5,
                    matchPlot: 0.3
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": {
                        "mongotQuery": {
                            "index": "search_index",
                            "text": {
                                "query": "mystery",
                                "path": "genres"
                            }
                        },
                        "requiresSearchSequenceToken": false,
                        "requiresSearchMetaCursor": true
                    }
                },
                {
                    "$match": {
                        "author": "dave"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 5
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$geoNear": {
                                    "near": [
                                        {
                                            "$const": 20
                                        },
                                        {
                                            "$const": 40
                                        }
                                    ],
                                    "query": {},
                                    "spherical": false
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$meta": "geoNearDistance"
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchDistance_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$search": {
                                    "mongotQuery": {
                                        "index": "search_index",
                                        "text": {
                                            "query": "mystery",
                                            "path": "genres"
                                        }
                                    },
                                    "requiresSearchSequenceToken": false,
                                    "requiresSearchMetaCursor": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 3
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$vectorSearch": {
                                    "queryVector": [
                                        1,
                                        2,
                                        3
                                    ],
                                    "path": "plot_embedding",
                                    "numCandidates": 300,
                                    "index": "vector_index",
                                    "limit": 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchPlot_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 0.3
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "matchDistance_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchDistance_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchGenres_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchPlot_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$matchAuthor_score",
                                "$matchDistance_score",
                                "$matchGenres_score",
                                "$matchPlot_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckIfWeightsArrayMixedIntsDecimals) {
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    name2: [
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
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    name1: 5,
                    name2: 3.2
                },
                method: "avg"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": { 
                        "mongotQuery": { 
                            "index": "search_index", 
                            "text": { "query": "mystery", "path": "genres" } 
                        }, 
                        "requiresSearchSequenceToken": false, 
                        "requiresSearchMetaCursor": true 
                    }
                },
                {
                    $match: {
                        author: "dave"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    $meta: "score"
                                },
                                {
                                    "$const": 5.0
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                $vectorSearch: {
                                    queryVector: [1.0, 2.0, 3.0],
                                    path: "plot_embedding",
                                    numCandidates: 300,
                                    index: "vector_index",
                                    limit: 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                $meta: "score"
                                            },
                                            {
                                                "$const": 3.2
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score",
                                "$name2_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {$meta: "score"},
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// There was a time when we had some parsing support for 'scoreNulls'. This test just confirms it is
// no longer an accepted argument.
TEST_F(DocumentSourceScoreFusionTest, ScoreNullsIsRejected) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    name1: 5
                },
                method: "avg"
            },
            scoreNulls: 0
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorIfOptionalFieldsIncludedMoreThanOnce) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    name1: 5
                },
                method: "avg",
                method: "duplicate"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLDuplicateField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfSearchMetaUsed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : { $add: [1.0, 4] }, normalization: "sigmoid" } }
                    ],
                    name2: [
                        {
                            $searchMeta: {
                                index: "search_index",
                                text: {
                                    query: "mystery",
                                    path: "genres"
                                }
                            }
                        },
                        { $score: { score : { $subtract: [4.0, 2] }, normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfSearchStoredSourceUsed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : 5.0, normalization: "sigmoid" } }
                    ],
                    name2: [
                        {
                            $search: {
                                index: "search_index",
                                text: {
                                    query: "mystery",
                                    path: "genres"
                                },
                                "returnStoredSource": true
                            }
                        },
                        { $sort: {genres: 1} }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInternalSearchMongotRemoteUsed) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : 5.0, normalization: "sigmoid" } }
                    ],
                    name2: [
                        {
                            $_internalSearchMongotRemote: {
                                index: "search_index",
                                text: {
                                    query: "mystery",
                                    path: "genres"
                                }
                            }
                        },
                        { $score: { score : 5.0, normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, CheckLimitSampleUnionWithNotAllowed) {
    auto expCtx = getExpCtx();
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto nsToUnionWith1 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $sample: { size: 10 } },
                        { $score: { score : 5.0, normalization: "sigmoid" } },
                        { $limit: 10 }
                    ],
                    name2: [
                        { $unionWith:
                            {
                                coll: "novels",
                                pipeline: [
                                    { $limit: 3 },
                                    {
                                        $unionWith: {
                                            coll: "shortstories"
                                        }
                                    }
                                ]
                            }
                        },
                        { $score: { score : 5.0, normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), expCtx),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNestedUnionWithModifiesFields) {
    auto expCtx = getExpCtx();
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto nsToUnionWith1 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $sample: { size: 10 } },
                        { $score: { score : 5.0, normalization: "sigmoid" } },
                        { $limit: 10 }
                    ],
                    name2: [
                        { $unionWith:
                            {
                                coll: "novels",
                                pipeline: [
                                    {
                                        $project: {
                                            _id: 1
                                        }
                                    },
                                    {
                                        $unionWith: {
                                            coll: "shortstories"
                                        }
                                    }
                                ]
                            }
                        },
                        { $score: { score : 5.0, normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), expCtx),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfIncludeProject) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : 5.0, normalization: "sigmoid" } }
                    ],
                    name2: [
                        { $match : { age : 50 } },
                        { $score: { score : 5.0, normalization: "sigmoid" } },
                        { $project: { author: 1 } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfPipelineNameDuplicated) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    foo: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: 5.0, normalization: "sigmoid" } }
                    ],
                    bar: [
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
                    foo: [
                        { $score: { score: 5.0, normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402203);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfPipelineNameStartsWithDollar) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    $matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: {score: 5.0, normalization: "sigmoid"} }
                    ],
                    matchGenres: [
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
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16410);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfPipelineNameContainsDot) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: {score: 5.0, normalization: "sigmoid"} }
                    ],
                    "match.genres": [
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
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16412);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfGeoNearPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    name2: [
                        {
                            $geoNear: {
                                near: { type: "Point", coordinates: [ -73.99279 , 40.719296 ] },
                                maxDistance: 2,
                                query: { category: "Parks" },
                                spherical: true
                            }
                        }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, CheckIfScoreWithGeoNearDistanceMetadataPipeline) {
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchGenres: [
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
                    matchDistance: [
                        {
                            $geoNear: {
                                near: { type: "Point", coordinates: [ -73.99279 , 40.719296 ] },
                                maxDistance: 2,
                                query: { category: "Parks" },
                                spherical: true
                            }
                        },
                        { $score: { score: { $meta: "geoNearDistance" }, normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$geoNear": {
                        "near": {
                            "type": {
                                "$const": "Point"
                            },
                            "coordinates": [
                                {
                                    "$const": -73.99279
                                },
                                {
                                    "$const": 40.719296
                                }
                            ]
                        },
                        "maxDistance": 2,
                        "query": {
                            "category": {
                                "$eq": "Parks"
                            }
                        },
                        "spherical": true
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$meta": "geoNearDistance"
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$exp": [
                                                {
                                                    "$multiply": [
                                                        {
                                                            "$const": -1
                                                        },
                                                        {
                                                            "$meta": "score"
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchDistance_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$search": {
                                    "mongotQuery": {
                                        "index": "search_index",
                                        "text": {
                                            "query": "mystery",
                                            "path": "genres"
                                        }
                                    },
                                    "requiresSearchSequenceToken": false,
                                    "requiresSearchMetaCursor": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "matchDistance_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchDistance_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchGenres_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$matchDistance_score",
                                "$matchGenres_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// Note: avg is the expected expression spelling, not 'average.'
TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInvalidCombinationMethodValue) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "average"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

// The remaining tests that specify "none" for combination.method desugar into the "avg" operation
// by default.
TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelinesAllowedAvgMethod) {
    auto fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    name2: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "avg"
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": { 
                        "mongotQuery": { 
                            "index": "search_index", 
                            "text": { "query": "mystery", "path": "genres" } 
                        }, 
                        "requiresSearchSequenceToken": false, 
                        "requiresSearchMetaCursor": true 
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$vectorSearch": {
                                    "queryVector": [
                                        1,
                                        2,
                                        3
                                    ],
                                    "path": "plot_embedding",
                                    "numCandidates": 300,
                                    "index": "vector_index",
                                    "limit": 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [ 
                                "$name1_score",
                                "$name2_score" 
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// This test should error since combination.method is expression but no combination.expression was
// specified.
TEST_F(DocumentSourceScoreFusionTest,
       ErrorsIfCombinationMethodExpressionButCombinationExpressionNotSpecified) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10017300);
}

// This test should error since combination.method is not expression but combination.expression was
// specified.
TEST_F(DocumentSourceScoreFusionTest,
       ErrorsIfCombinationMethodNotExpressionButCombinationExpressionSpecified) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "avg",
                expression: {$sum: ["$$name1", 5.0]}
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10017300);
}

// This test should error since combination.weights and combination.expression are specified at the
// same time.
TEST_F(DocumentSourceScoreFusionTest,
       ErrorsIfValidCombinationMethodExpressionButCombinationWeightsAndExpressionSpecified) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    name1: 5
                },
                method: "expression",
                expression: {$sum: ["$$name1", 5.0]}
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10017301);
}

// This test specifies an expression that references the only pipeline.
TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineAllowedNormalizationNoneMethodExpression) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$sum: ["$$name1", 5.0]}
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$name1_score"
                                },
                                "in": {
                                    "$sum": [
                                        "$$name1",
                                        {
                                            "$const": 5
                                        }
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// This test specifies an expression that that doesn't reference any of the pipelines.
TEST_F(DocumentSourceScoreFusionTest,
       CheckOnePipelineAllowedNormalizationNoneMethodConstExpression) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$const: 1.0}
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$name1_score"
                                },
                                "in": {
                                    "$const": 1
                                }
                            }
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// This test specifies an expression that references both pipelines.
TEST_F(DocumentSourceScoreFusionTest,
       CheckMultiplePipelinesAllowedNormalizationNoneMethodExpression) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    name2: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$sum: ["$$name1", "$$name2", 5.0]}
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": { 
                        "mongotQuery": { 
                            "index": "search_index", 
                            "text": { "query": "mystery", "path": "genres" } 
                        }, 
                        "requiresSearchSequenceToken": false, 
                        "requiresSearchMetaCursor": true 
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$vectorSearch": {
                                    "queryVector": [
                                        1,
                                        2,
                                        3
                                    ],
                                    "path": "plot_embedding",
                                    "numCandidates": 300,
                                    "index": "vector_index",
                                    "limit": 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$name1_score",
                                    "name2": "$name2_score"
                                },
                                "in": {
                                    "$sum": [
                                        "$$name1",
                                        "$$name2",
                                        {
                                            "$const": 5
                                        }
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// This test specifies only one of the two pipeline names. This should still desugar correctly.
TEST_F(DocumentSourceScoreFusionTest,
       CheckMultiplePipelinesAllowedNormalizationNoneMethodExpressionNotAllPipelineVarsSpecified) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    name2: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$sum: ["$$name1", 5.0]}
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": { 
                        "mongotQuery": { 
                            "index": "search_index", 
                            "text": { "query": "mystery", "path": "genres" } 
                        }, 
                        "requiresSearchSequenceToken": false, 
                        "requiresSearchMetaCursor": true 
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$vectorSearch": {
                                    "queryVector": [
                                        1,
                                        2,
                                        3
                                    ],
                                    "path": "plot_embedding",
                                    "numCandidates": 300,
                                    "index": "vector_index",
                                    "limit": 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$name1_score",
                                    "name2": "$name2_score"
                                },
                                "in": {
                                    "$sum": [
                                        "$$name1",
                                        {
                                            "$const": 5
                                        }
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// This test specifies "name3" instead of the pipeline called "name2." This should error because
// "name3" is an undefined variable so the $let expression cannot be parsed.
TEST_F(DocumentSourceScoreFusionTest,
       ErrorsIfMultiplePipelinesAllowedNormalizationNoneMethodExpressionSpecifiesWrongPipeline) {
    // This is needed because the $unionWith stage still gets constructed and getResolvedNamespace
    // is called.
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    name2: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$sum: ["$$name1", "$$name3", 5.0]}
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17276);
}

// This test specifies an expression that doesn't evaluate to a numerical value. Note that this will
// still desugar properly but a run-time error will be thrown when the desugared output is
// evaluated. See score_fusion_score_combination_test.js for the corresponding testcase that
// verifies an error is thrown when combination.expression evaluates to a nonnumerical value.
TEST_F(DocumentSourceScoreFusionTest,
       CheckOnePipelineAllowedNormalizationNoneMethodNonNumericalExpression) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$toString: 2.5}
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$name1_score"
                                },
                                "in": {
                                    "$convert": {
                                        "input": {
                                            "$const": 2.5
                                        },
                                        "to": {
                                            "$const": "string"
                                        },
                                        "format": {
                                            "$const": "auto"
                                        }
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

// This test specifies a $let expression which means the resulting desugared combination.expression
// winds up becoming a nested $let expression.
TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineAllowedNormalizationNoneMethodLetExpression) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$let: {vars: {name1: "$name1_score"}, 
                in: {$sum: ["$$name1", {$const: 5}]}}}
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        $willBeMerged: false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$name1_score"
                                },
                                "in": {
                                    "$let": {
                                        "vars": {
                                            "name1": "$name1_score"
                                        },
                                        "in": {
                                            "$sum": [
                                                "$$name1",
                                                {
                                                    "$const": 5
                                                }
                                            ]
                                        }
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineScoreDetailsDesugaring) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    name1: 5
                }
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 5
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name1_scoreDetails": {
                            "$mergeObjects": "$name1_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "name1"
                                        },
                                        "inputPipelineRawScore": "$name1_rawScore",
                                        "weight": {
                                            "$const": 5
                                        },
                                        "value": "$name1_score"
                                    },
                                    "$name1_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "none"
                            },
                            "combination": {
                                "method": {
                                    "$const": "average"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckTwoPipelineSearchWithScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    searchPipe: [
                        {
                            $search: {
                                index: "search_index",
                                text: {
                                    query: "mystery",
                                    path: "genres"
                                },
                                scoreDetails: true
                            }
                        }
                    ]
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    name1: 3,
                    searchPipe: 2
                }
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 3
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$search": {
                                    "mongotQuery": {
                                        "index": "search_index",
                                        "text": {
                                            "query": "mystery",
                                            "path": "genres"
                                        },
                                        "scoreDetails": true
                                    },
                                    "requiresSearchSequenceToken": false,
                                    "requiresSearchMetaCursor": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 2
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_scoreDetails": {
                                        "details": {
                                            "$meta": "scoreDetails"
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name1_scoreDetails": {
                            "$mergeObjects": "$name1_scoreDetails"
                        },
                        "searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$searchPipe_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "searchPipe_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$searchPipe_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "searchPipe_scoreDetails": {
                            "$mergeObjects": "$searchPipe_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$name1_score",
                                "$searchPipe_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "name1"
                                        },
                                        "inputPipelineRawScore": "$name1_rawScore",
                                        "weight": {
                                            "$const": 3
                                        },
                                        "value": "$name1_score"
                                    },
                                    "$name1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "searchPipe"
                                        },
                                        "inputPipelineRawScore": "$searchPipe_rawScore",
                                        "weight": {
                                            "$const": 2
                                        },
                                        "value": "$searchPipe_score"
                                    },
                                    "$searchPipe_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "none"
                            },
                            "combination": {
                                "method": {
                                    "$const": "average"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckTwoPipelineSearchNoScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    search: [
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
                    vector: [
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
                },
                normalization: "none"
            },
            combination: {
                weights: {
                    vector: 1,
                    search: 2
                }
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": {
                        "mongotQuery": {
                            "index": "search_index",
                            "text": {
                                "query": "mystery",
                                "path": "genres"
                            }
                        },
                        "requiresSearchSequenceToken": false,
                        "requiresSearchMetaCursor": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "search_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 2
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "search_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "search_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$vectorSearch": {
                                    "queryVector": [
                                        1,
                                        2,
                                        3
                                    ],
                                    "path": "plot_embedding",
                                    "numCandidates": 300,
                                    "index": "vector_index",
                                    "limit": 10
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "vector_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "vector_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "vector_scoreDetails": {
                                        "details": []
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "search_score": {
                            "$max": {
                                "$ifNull": [
                                    "$search_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "search_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$search_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "search_scoreDetails": {
                            "$mergeObjects": "$search_scoreDetails"
                        },
                        "vector_score": {
                            "$max": {
                                "$ifNull": [
                                    "$vector_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "vector_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$vector_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "vector_scoreDetails": {
                            "$mergeObjects": "$vector_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$search_score",
                                "$vector_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "search"
                                        },
                                        "inputPipelineRawScore": "$search_rawScore",
                                        "weight": {
                                            "$const": 2
                                        },
                                        "value": "$search_score"
                                    },
                                    "$search_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "vector"
                                        },
                                        "inputPipelineRawScore": "$vector_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$vector_score"
                                    },
                                    "$vector_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "none"
                            },
                            "combination": {
                                "method": {
                                    "$const": "average"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckTwoPipelineCustomExpressionScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
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
                    searchPipe: [
                        {
                            $search: {
                                index: "search_index",
                                text: {
                                    query: "mystery",
                                    path: "genres"
                                },
                                scoreDetails: true
                            }
                        }
                    ]
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: {$sum: ["$$name1", 5.0]}
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$vectorSearch": {
                        "queryVector": [
                            1,
                            2,
                            3
                        ],
                        "path": "plot_embedding",
                        "numCandidates": 300,
                        "index": "vector_index",
                        "limit": 10
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "name1_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$search": {
                                    "mongotQuery": {
                                        "index": "search_index",
                                        "text": {
                                            "query": "mystery",
                                            "path": "genres"
                                        },
                                        "scoreDetails": true
                                    },
                                    "requiresSearchSequenceToken": false,
                                    "requiresSearchMetaCursor": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_scoreDetails": {
                                        "details": {
                                            "$meta": "scoreDetails"
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$name1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "name1_scoreDetails": {
                            "$mergeObjects": "$name1_scoreDetails"
                        },
                        "searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$searchPipe_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "searchPipe_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$searchPipe_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "searchPipe_scoreDetails": {
                            "$mergeObjects": "$searchPipe_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$name1_score",
                                    "searchPipe": "$searchPipe_score"
                                },
                                "in": {
                                    "$sum": [
                                        "$$name1",
                                        {
                                            "$const": 5
                                        }
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "name1"
                                        },
                                        "inputPipelineRawScore": "$name1_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$name1_score"
                                    },
                                    "$name1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "searchPipe"
                                        },
                                        "inputPipelineRawScore": "$searchPipe_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$searchPipe_score"
                                    },
                                    "$searchPipe_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "none"
                            },
                            "combination": {
                                "method": {
                                    "$const": "custom expression"
                                },
                                "expression": {
                                    "$const": "{ string: { $sum: [ '$$name1', 5.0 ] } }"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckTwoPipelineScoreInputPipelineScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    scorePipe2: [
                        { $score: { score: { $add: [10, 2] }, normalization: "sigmoid" } }
                    ]
                },
                normalization: "none"
            },
            combination: {
                method: "avg"
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$age"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$add": [
                                            {
                                                "$const": 10
                                            },
                                            {
                                                "$const": 2
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$exp": [
                                                            {
                                                                "$multiply": [
                                                                    {
                                                                        "$const": -1
                                                                    },
                                                                    {
                                                                        "$meta": "score"
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_scoreDetails": {
                                        "details": []
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_scoreDetails": {
                            "$mergeObjects": "$scorePipe1_scoreDetails"
                        },
                        "scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_scoreDetails": {
                            "$mergeObjects": "$scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$scorePipe1_score",
                                "$scorePipe2_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe1"
                                        },
                                        "inputPipelineRawScore": "$scorePipe1_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe1_score"
                                    },
                                    "$scorePipe1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe2"
                                        },
                                        "inputPipelineRawScore": "$scorePipe2_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe2_score"
                                    },
                                    "$scorePipe2_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "none"
                            },
                            "combination": {
                                "method": {
                                    "$const": "average"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreInputPipelineSigmoidScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    scorePipe2: [
                        { $score: { score: { $add: [10, 2] }, normalization: "none" } }
                    ]
                },
                normalization: "sigmoid"
            },
            combination: {
                method: "avg"
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$age"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                {
                                                    "$const": 1
                                                },
                                                {
                                                    "$exp": [
                                                        {
                                                            "$multiply": [
                                                                {
                                                                    "$const": -1
                                                                },
                                                                {
                                                                    "$meta": "score"
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$add": [
                                            {
                                                "$const": 10
                                            },
                                            {
                                                "$const": 2
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            {
                                                                "$const": 1
                                                            },
                                                            {
                                                                "$exp": [
                                                                    {
                                                                        "$multiply": [
                                                                            {
                                                                                "$const": -1
                                                                            },
                                                                            {
                                                                                "$meta": "score"
                                                                            }
                                                                        ]
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_scoreDetails": {
                                        "details": []
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_scoreDetails": {
                            "$mergeObjects": "$scorePipe1_scoreDetails"
                        },
                        "scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_scoreDetails": {
                            "$mergeObjects": "$scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$scorePipe1_score",
                                "$scorePipe2_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe1"
                                        },
                                        "inputPipelineRawScore": "$scorePipe1_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe1_score"
                                    },
                                    "$scorePipe1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe2"
                                        },
                                        "inputPipelineRawScore": "$scorePipe2_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe2_score"
                                    },
                                    "$scorePipe2_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "sigmoid"
                            },
                            "combination": {
                                "method": {
                                    "$const": "average"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreScoreDetailsInputPipelineScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "none", scoreDetails: true } }
                    ],
                    scorePipe2: [
                        { $score: {
                            score: { $add: [10, 2] },
                            normalization: "sigmoid",
                            weight: 0.5,
                            scoreDetails: true }
                        }
                    ]
                },
                normalization: "sigmoid"
            },
            combination: {
                method: "avg"
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    std::string expectedStages = std::string(R"({
            "expectedStages": [
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$age"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                            },
                            "rawScore": "$internal_raw_score",
                            "normalization": {
                                "$const": "none"
                            },
                            "weight": {
                                "$const": 1
                            },
                            "expression": {
                                "$const": "{ string: '$age' }"
                            },
                            "details": []
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                {
                                                    "$const": 1
                                                },
                                                {
                                                    "$exp": [
                                                        {
                                                            "$multiply": [
                                                                {
                                                                    "$const": -1
                                                                },
                                                                {
                                                                    "$meta": "score"
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$addFields": {
                        "scorePipe1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_scoreDetails": {
                            "details": {
                                "$meta": "scoreDetails"
                            }
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$add": [
                                            {
                                                "$const": 10
                                            },
                                            {
                                                "$const": 2
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$exp": [
                                                            {
                                                                "$multiply": [
                                                                    {
                                                                        "$const": -1
                                                                    },
                                                                    {
                                                                        "$meta": "score"
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 0.5
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "scoreDetails": {
                                        "value": {
                                            "$meta": "score"
                                        },
                                        "description": {
                                            "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                                        },
                                        "rawScore": "$internal_raw_score",
                                        "normalization": {
                                            "$const": "sigmoid"
                                        },
                                        "weight": {
                                            "$const": 0.5
                                        },
                                        "expression": {
                                            "$const": "{ string: { $add: [ 10, 2 ] } }"
                                        },
                                        "details": []
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            {
                                                                "$const": 1
                                                            },
                                                            {
                                                                "$exp": [
                                                                    {
                                                                        "$multiply": [
                                                                            {
                                                                                "$const": -1
                                                                            },
                                                                            {
                                                                                "$meta": "score"
                                                                            }
                                                                        ]
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_scoreDetails": {
                                        "details": {
                                            "$meta": "scoreDetails"
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },)") +
        std::string(R"(
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_scoreDetails": {
                            "$mergeObjects": "$scorePipe1_scoreDetails"
                        },
                        "scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_scoreDetails": {
                            "$mergeObjects": "$scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$scorePipe1_score",
                                "$scorePipe2_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe1"
                                        },
                                        "inputPipelineRawScore": "$scorePipe1_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe1_score"
                                    },
                                    "$scorePipe1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe2"
                                        },
                                        "inputPipelineRawScore": "$scorePipe2_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe2_score"
                                    },
                                    "$scorePipe2_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "sigmoid"
                            },
                            "combination": {
                                "method": {
                                    "$const": "average"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ(fromjson(expectedStages), asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreInputPipelineMinMaxScalerExpressionScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    scorePipe2: [
                        { $score: { score: { $add: [10, 2] }, normalization: "none" } }
                    ]
                },
                normalization: "minMaxScaler"
            },
            combination: {
                method: "expression",
                expression: {$sum: ["$$scorePipe1", "$$scorePipe2", 5.0]}
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$age"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "scorePipe1_score": -1
                        },
                        "output": {
                            "scorePipe1_score": {
                                "$minMaxScaler": {
                                    "input": "$scorePipe1_score",
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$add": [
                                            {
                                                "$const": 10
                                            },
                                            {
                                                "$const": 2
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_scoreDetails": {
                                        "details": []
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "scorePipe2_score": -1
                                    },
                                    "output": {
                                        "scorePipe2_score": {
                                            "$minMaxScaler": {
                                                "input": "$scorePipe2_score",
                                                "min": 0,
                                                "max": 1
                                            },
                                            "window": {
                                                "documents": [
                                                    "unbounded",
                                                    "unbounded"
                                                ]
                                            }
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_scoreDetails": {
                            "$mergeObjects": "$scorePipe1_scoreDetails"
                        },
                        "scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_scoreDetails": {
                            "$mergeObjects": "$scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "scorePipe1": "$scorePipe1_score",
                                    "scorePipe2": "$scorePipe2_score"
                                },
                                "in": {
                                    "$sum": [
                                        "$$scorePipe1",
                                        "$$scorePipe2",
                                        {
                                            "$const": 5
                                        }
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe1"
                                        },
                                        "inputPipelineRawScore": "$scorePipe1_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe1_score"
                                    },
                                    "$scorePipe1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe2"
                                        },
                                        "inputPipelineRawScore": "$scorePipe2_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe2_score"
                                    },
                                    "$scorePipe2_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "minMaxScaler"
                            },
                            "combination": {
                                "method": {
                                    "$const": "custom expression"
                                },
                                "expression": {
                                    "$const": "{ string: { $sum: [ '$$scorePipe1', '$$scorePipe2', 5.0 ] } }"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}
TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreInputPipelineMinMaxScalerUnspecifiedCombinationScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    scorePipe2: [
                        { $score: { score: { $add: [10, 2] }, normalization: "none" } }
                    ]
                },
                normalization: "minMaxScaler"
            },
            combination: {},
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$age"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_scoreDetails": {
                            "details": []
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "scorePipe1_score": -1
                        },
                        "output": {
                            "scorePipe1_score": {
                                "$minMaxScaler": {
                                    "input": "$scorePipe1_score",
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$add": [
                                            {
                                                "$const": 10
                                            },
                                            {
                                                "$const": 2
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_scoreDetails": {
                                        "details": []
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "scorePipe2_score": -1
                                    },
                                    "output": {
                                        "scorePipe2_score": {
                                            "$minMaxScaler": {
                                                "input": "$scorePipe2_score",
                                                "min": 0,
                                                "max": 1
                                            },
                                            "window": {
                                                "documents": [
                                                    "unbounded",
                                                    "unbounded"
                                                ]
                                            }
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_scoreDetails": {
                            "$mergeObjects": "$scorePipe1_scoreDetails"
                        },
                        "scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_scoreDetails": {
                            "$mergeObjects": "$scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$scorePipe1_score",
                                "$scorePipe2_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe1"
                                        },
                                        "inputPipelineRawScore": "$scorePipe1_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe1_score"
                                    },
                                    "$scorePipe1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe2"
                                        },
                                        "inputPipelineRawScore": "$scorePipe2_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe2_score"
                                    },
                                    "$scorePipe2_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "minMaxScaler"
                            },
                            "combination": {
                                "method": {
                                    "$const": "average"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreWithScoreDetailsInputPipelinesScoreDetailsDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        { $geoNear : { near : [20, 40] } },
                        { $score: { score: { $meta: "geoNearDistance"} , normalization: "none", scoreDetails: true } }
                    ],
                    scorePipe2: [
                        { $score: { score: { $add: [10, 2] }, normalization: "none", scoreDetails: true } }
                    ]
                },
                normalization: "sigmoid"
            },
            combination: {
                method: "expression",
                expression: { $add: [ { $multiply: [ "$$scorePipe1", 0.5 ] }, "$$scorePipe2"] }
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    std::string expectedStages = std::string(R"({
            "expectedStages": [
                        {
                            "$geoNear": {
                                "near": [
                                    {
                                        "$const": 20
                                    },
                                    {
                                        "$const": 40
                                    }
                                ],
                                "query": {},
                                "spherical": false
                            }
                        },
                        {
                            "$setMetadata": {
                                "score": {
                                    "$meta": "geoNearDistance"
                                }
                            }
                        },
                        {
                            "$replaceRoot": {
                                "newRoot": {
                                    "docs": "$$ROOT"
                                }
                            }
                        },
                        {
                            "$addFields": {
                                "internal_raw_score": {
                                    "$meta": "score"
                                }
                            }
                        },
                        {
                            "$setMetadata": {
                                "scoreDetails": {
                                    "value": {
                                        "$meta": "score"
                                    },
                                    "description": {
                                        "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                                    },
                                    "rawScore": "$internal_raw_score",
                                    "normalization": {
                                        "$const": "none"
                                    },
                                    "weight": {
                                        "$const": 1
                                    },
                                    "expression": {
                                        "$const": "{ string: { $meta: 'geoNearDistance' } }"
                                    },
                                    "details": []
                                }
                            }
                        },
                        {
                            "$replaceRoot": {
                                "newRoot": "$docs"
                            }
                        },
                        {
                            "$replaceRoot": {
                                "newRoot": {
                                    "docs": "$$ROOT"
                                }
                            }
                        },
                        {
                            "$addFields": {
                                "scorePipe1_score": {
                                    "$multiply": [
                                        {
                                            "$divide": [
                                                {
                                                    "$const": 1
                                                },
                                                {
                                                    "$add": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$exp": [
                                                                {
                                                                    "$multiply": [
                                                                        {
                                                                            "$const": -1
                                                                        },
                                                                        {
                                                                            "$meta": "score"
                                                                        }
                                                                    ]
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                }
                                            ]
                                        },
                                        {
                                            "$const": 1
                                        }
                                    ]
                                }
                            }
                        },)") +
        std::string(R"(
                        {
                            "$addFields": {
                                "scorePipe1_rawScore": {
                                    "$meta": "score"
                                }
                            }
                        },
                        {
                            "$addFields": {
                                "scorePipe1_scoreDetails": {
                                    "details": {
                                        "$meta": "scoreDetails"
                                    }
                                }
                            }
                        },
                        {
                            "$unionWith": {
                                "coll": "pipeline_test",
                                "pipeline": [
                                    {
                                        "$setMetadata": {
                                            "score": {
                                                "$add": [
                                                    {
                                                        "$const": 10
                                                    },
                                                    {
                                                        "$const": 2
                                                    }
                                                ]
                                            }
                                        }
                                    },
                                    {
                                        "$replaceRoot": {
                                            "newRoot": {
                                                "docs": "$$ROOT"
                                            }
                                        }
                                    },
                                    {
                                        "$addFields": {
                                            "internal_raw_score": {
                                                "$meta": "score"
                                            }
                                        }
                                    },
                                    {
                                        "$setMetadata": {
                                            "scoreDetails": {
                                                "value": {
                                                    "$meta": "score"
                                                },
                                                "description": {
                                                    "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                                                },
                                                "rawScore": "$internal_raw_score",
                                                "normalization": {
                                                    "$const": "none"
                                                },
                                                "weight": {
                                                    "$const": 1
                                                },
                                                "expression": {
                                                    "$const": "{ string: { $add: [ 10, 2 ] } }"
                                                },
                                                "details": []
                                            }
                                        }
                                    },
                                    {
                                        "$replaceRoot": {
                                            "newRoot": "$docs"
                                        }
                                    },
                                    {
                                        "$replaceRoot": {
                                            "newRoot": {
                                                "docs": "$$ROOT"
                                            }
                                        }
                                    },
                                    {
                                        "$addFields": {
                                            "scorePipe2_score": {
                                                "$multiply": [
                                                    {
                                                        "$divide": [
                                                            {
                                                                "$const": 1
                                                            },
                                                            {
                                                                "$add": [
                                                                    {
                                                                        "$const": 1
                                                                    },
                                                                    {
                                                                        "$exp": [
                                                                            {
                                                                                "$multiply": [
                                                                                    {
                                                                                        "$const": -1
                                                                                    },
                                                                                    {
                                                                                        "$meta": "score"
                                                                                    }
                                                                                ]
                                                                            }
                                                                        ]
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    },
                                                    {
                                                        "$const": 1
                                                    }
                                                ]
                                            }
                                        }
                                    },
                                    {
                                        "$addFields": {
                                            "scorePipe2_rawScore": {
                                                "$meta": "score"
                                            }
                                        }
                                    },
                                    {
                                        "$addFields": {
                                            "scorePipe2_scoreDetails": {
                                                "details": {
                                                    "$meta": "scoreDetails"
                                                }
                                            }
                                        }
                                    }
                                ]
                            }
                        },)") +
        std::string(R"(
                        {
                            "$group": {
                                "_id": "$docs._id",
                                "docs": {
                                    "$first": "$docs"
                                },
                                "scorePipe1_score": {
                                    "$max": {
                                        "$ifNull": [
                                            "$scorePipe1_score",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    }
                                },
                                "scorePipe1_rawScore": {
                                    "$max": {
                                        "$ifNull": [
                                            "$scorePipe1_rawScore",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    }
                                },
                                "scorePipe1_scoreDetails": {
                                    "$mergeObjects": "$scorePipe1_scoreDetails"
                                },
                                "scorePipe2_score": {
                                    "$max": {
                                        "$ifNull": [
                                            "$scorePipe2_score",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    }
                                },
                                "scorePipe2_rawScore": {
                                    "$max": {
                                        "$ifNull": [
                                            "$scorePipe2_rawScore",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    }
                                },
                                "scorePipe2_scoreDetails": {
                                    "$mergeObjects": "$scorePipe2_scoreDetails"
                                },
                                "$willBeMerged": false
                            }
                        },
                        {
                            "$setMetadata": {
                                "score": {
                                    "$let": {
                                        "vars": {
                                            "scorePipe1": "$scorePipe1_score",
                                            "scorePipe2": "$scorePipe2_score"
                                        },
                                        "in": {
                                            "$add": [
                                                {
                                                    "$multiply": [
                                                        "$$scorePipe1",
                                                        {
                                                            "$const": 0.5
                                                        }
                                                    ]
                                                },
                                                "$$scorePipe2"
                                            ]
                                        }
                                    }
                                }
                            }
                        },
                        {
                            "$addFields": {
                                "calculatedScoreDetails": [
                                    {
                                        "$mergeObjects": [
                                            {
                                                "inputPipelineName": {
                                                    "$const": "scorePipe1"
                                                },
                                                "inputPipelineRawScore": "$scorePipe1_rawScore",
                                                "weight": {
                                                    "$const": 1
                                                },
                                                "value": "$scorePipe1_score"
                                            },
                                            "$scorePipe1_scoreDetails"
                                        ]
                                    },
                                    {
                                        "$mergeObjects": [
                                            {
                                                "inputPipelineName": {
                                                    "$const": "scorePipe2"
                                                },
                                                "inputPipelineRawScore": "$scorePipe2_rawScore",
                                                "weight": {
                                                    "$const": 1
                                                },
                                                "value": "$scorePipe2_score"
                                            },
                                            "$scorePipe2_scoreDetails"
                                        ]
                                    }
                                ]
                            }
                        },
                        {
                            "$setMetadata": {
                                "scoreDetails": {
                                    "value": {
                                        "$meta": "score"
                                    },
                                    "description": {
                                        "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                                    },
                                    "normalization": {
                                        "$const": "sigmoid"
                                    },
                                    "combination": {
                                        "method": {
                                            "$const": "custom expression"
                                        },
                                        "expression": {
                                            "$const": "{ string: { $add: [ { $multiply: [ '$$scorePipe1', 0.5 ] }, '$$scorePipe2' ] } }"
                                        }
                                    },
                                    "details": "$calculatedScoreDetails"
                                }
                            }
                        },
                        {
                            "$sort": {
                                "$computed0": {
                                    "$meta": "score"
                                },
                                "_id": 1
                            }
                        },
                        {
                            "$replaceRoot": {
                                "newRoot": "$docs"
                            }
                        }
                    ]
                })");
    ASSERT_BSONOBJ_EQ(fromjson(expectedStages), asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreWithScoreDetailsInputPipelinesScoreDetailsMinMaxScalerDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    scorePipe1: [
                        { $geoNear : { near : [20, 40] } },
                        { $score: { score: { $meta: "geoNearDistance"} , normalization: "none", scoreDetails: true } }
                    ],
                    scorePipe2: [
                        { $score: { score: { $add: [10, 2] }, normalization: "none", scoreDetails: true } }
                    ]
                },
                normalization: "minMaxScaler"
            },
            combination: {
                method: "expression",
                expression: { $add: [ { $multiply: [ "$$scorePipe1", 0.5 ] }, "$$scorePipe2"] }
            },
            scoreDetails: true
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    std::string expectedStages = std::string(R"({
            "expectedStages": [
                {
                    "$geoNear": {
                        "near": [
                            {
                                "$const": 20
                            },
                            {
                                "$const": 40
                            }
                        ],
                        "query": {},
                        "spherical": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$meta": "geoNearDistance"
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                            },
                            "rawScore": "$internal_raw_score",
                            "normalization": {
                                "$const": "none"
                            },
                            "weight": {
                                "$const": 1
                            },
                            "expression": {
                                "$const": "{ string: { $meta: 'geoNearDistance' } }"
                            },
                            "details": []
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 1
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "scorePipe1_scoreDetails": {
                            "details": {
                                "$meta": "scoreDetails"
                            }
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "scorePipe1_score": -1
                        },
                        "output": {
                            "scorePipe1_score": {
                                "$minMaxScaler": {
                                    "input": "$scorePipe1_score",
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$add": [
                                            {
                                                "$const": 10
                                            },
                                            {
                                                "$const": 2
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "scoreDetails": {
                                        "value": {
                                            "$meta": "score"
                                        },
                                        "description": {
                                            "$const": "the score calculated from multiplying a weight in the range [0,1] with either a normalized or nonnormalized value:"
                                        },
                                        "rawScore": "$internal_raw_score",
                                        "normalization": {
                                            "$const": "none"
                                        },
                                        "weight": {
                                            "$const": 1
                                        },
                                        "expression": {
                                            "$const": "{ string: { $add: [ 10, 2 ] } }"
                                        },
                                        "details": []
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 1
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "scorePipe2_scoreDetails": {
                                        "details": {
                                            "$meta": "scoreDetails"
                                        }
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "scorePipe2_score": -1
                                    },
                                    "output": {
                                        "scorePipe2_score": {
                                            "$minMaxScaler": {
                                                "input": "$scorePipe2_score",
                                                "min": 0,
                                                "max": 1
                                            },
                                            "window": {
                                                "documents": [
                                                    "unbounded",
                                                    "unbounded"
                                                ]
                                            }
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },)") +
        std::string(R"(
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe1_scoreDetails": {
                            "$mergeObjects": "$scorePipe1_scoreDetails"
                        },
                        "scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "scorePipe2_scoreDetails": {
                            "$mergeObjects": "$scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "scorePipe1": "$scorePipe1_score",
                                    "scorePipe2": "$scorePipe2_score"
                                },
                                "in": {
                                    "$add": [
                                        {
                                            "$multiply": [
                                                "$$scorePipe1",
                                                {
                                                    "$const": 0.5
                                                }
                                            ]
                                        },
                                        "$$scorePipe2"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "calculatedScoreDetails": [
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe1"
                                        },
                                        "inputPipelineRawScore": "$scorePipe1_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe1_score"
                                    },
                                    "$scorePipe1_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "scorePipe2"
                                        },
                                        "inputPipelineRawScore": "$scorePipe2_rawScore",
                                        "weight": {
                                            "$const": 1
                                        },
                                        "value": "$scorePipe2_score"
                                    },
                                    "$scorePipe2_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "description": {
                                "$const": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:"
                            },
                            "normalization": {
                                "$const": "minMaxScaler"
                            },
                            "combination": {
                                "method": {
                                    "$const": "custom expression"
                                },
                                "expression": {
                                    "$const": "{ string: { $add: [ { $multiply: [ '$$scorePipe1', 0.5 ] }, '$$scorePipe2' ] } }"
                                }
                            },
                            "details": "$calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ(fromjson(expectedStages), asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreSigmoidInputPipelinesScoreFusionSigmoidDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    single: [
                        { $score: { score: '$single' , normalization: "sigmoid" } }
                    ],
                    double: [
                        { $score: { score: '$double' , normalization: "sigmoid" } }
                    ]
                },
                normalization: "sigmoid"
            },
            combination: {
                weights: {
                    single: 0.5,
                    double: 2
                },
                method: "avg"
            }
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    std::string expectedStages = std::string(R"({
            "expectedStages": [
                {
                    "$setMetadata": {
                        "score": "$double"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$exp": [
                                                {
                                                    "$multiply": [
                                                        {
                                                            "$const": -1
                                                        },
                                                        {
                                                            "$meta": "score"
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                }
                            ]
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "double_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                {
                                                    "$const": 1
                                                },
                                                {
                                                    "$exp": [
                                                        {
                                                            "$multiply": [
                                                                {
                                                                    "$const": -1
                                                                },
                                                                {
                                                                    "$meta": "score"
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                }
                                            ]
                                        }
                                    ]
                                },
                                {
                                    "$const": 2
                                }
                            ]
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": "$single"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$exp": [
                                                            {
                                                                "$multiply": [
                                                                    {
                                                                        "$const": -1
                                                                    },
                                                                    {
                                                                        "$meta": "score"
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "single_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            {
                                                                "$const": 1
                                                            },
                                                            {
                                                                "$exp": [
                                                                    {
                                                                        "$multiply": [
                                                                            {
                                                                                "$const": -1
                                                                            },
                                                                            {
                                                                                "$meta": "score"
                                                                            }
                                                                        ]
                                                                    }
                                                                ]
                                                            }
                                                        ]
                                                    }
                                                ]
                                            },
                                            {
                                                "$const": 0.5
                                            }
                                        ]
                                    }
                                }
                            }
                        ]
                    }
                },)") +
        std::string(R"(
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "double_score": {
                            "$max": {
                                "$ifNull": [
                                    "$double_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "single_score": {
                            "$max": {
                                "$ifNull": [
                                    "$single_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$double_score",
                                "$single_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ(fromjson(expectedStages), asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest,
       CheckTwoPipelineScoreMinMaxScalerInputPipelinesScoreFusionMinMaxScalerDesugaring) {
    NamespaceString fromNs = NamespaceString::createNamespaceString_forTest("test.pipeline_test");
    getExpCtx()->setResolvedNamespaces(
        ResolvedNamespaceMap{{fromNs, {fromNs, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    single: [
                        { $score: { score: '$single' , normalization: "minMaxScaler" } }
                    ],
                    double: [
                        { $score: { score: '$double' , normalization: "minMaxScaler" } }
                    ]
                },
                normalization: "minMaxScaler"
            },
            combination: {
                weights: {
                    single: 0.5,
                    double: 2
                },
                method: "avg"
            }
        }
     })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    std::string expectedStages = std::string(R"({
            "expectedStages": [
                {
                    "$setMetadata": {
                        "score": "$double"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "internal_raw_score": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "internal_min_max_scaler_normalization_score": -1
                        },
                        "output": {
                            "internal_min_max_scaler_normalization_score": {
                                "$minMaxScaler": {
                                    "input": {
                                        "$meta": "score"
                                    },
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$internal_min_max_scaler_normalization_score"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "double_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                {
                                    "$const": 2
                                }
                            ]
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "double_score": -1
                        },
                        "output": {
                            "double_score": {
                                "$minMaxScaler": {
                                    "input": "$double_score",
                                    "min": 0,
                                    "max": 1
                                },
                                "window": {
                                    "documents": [
                                        "unbounded",
                                        "unbounded"
                                    ]
                                }
                            }
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$setMetadata": {
                                    "score": "$single"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "internal_raw_score": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "internal_min_max_scaler_normalization_score": -1
                                    },
                                    "output": {
                                        "internal_min_max_scaler_normalization_score": {
                                            "$minMaxScaler": {
                                                "input": {
                                                    "$meta": "score"
                                                },
                                                "min": 0,
                                                "max": 1
                                            },
                                            "window": {
                                                "documents": [
                                                    "unbounded",
                                                    "unbounded"
                                                ]
                                            }
                                        }
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": "$internal_min_max_scaler_normalization_score"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": "$docs"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "single_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            {
                                                "$const": 0.5
                                            }
                                        ]
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "single_score": -1
                                    },
                                    "output": {
                                        "single_score": {
                                            "$minMaxScaler": {
                                                "input": "$single_score",
                                                "min": 0,
                                                "max": 1
                                            },
                                            "window": {
                                                "documents": [
                                                    "unbounded",
                                                    "unbounded"
                                                ]
                                            }
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },)") +
        std::string(R"(
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "double_score": {
                            "$max": {
                                "$ifNull": [
                                    "$double_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "single_score": {
                            "$max": {
                                "$ifNull": [
                                    "$single_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$double_score",
                                "$single_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$docs"
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ(fromjson(expectedStages), asOneObj);
}
}  // namespace
}  // namespace mongo
