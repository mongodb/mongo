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

#include "mongo/unittest/unittest.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_score.h"
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class DocumentSourceScoreFusionTest : service_context_test::WithSetupTransportLayer,
                                      public AggregationContextFixture {};

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNoInputsField) {
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
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInputsIsNotObject) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {pipelines: "not an object"},
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
            inputs: {
                pipelines: {}
            },
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
            inputs: {},
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineAllowed) {
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            inputNormalization: "none"
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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
            inputs: {
                pipelines: {
                    authorMatch: {
                        $match : { author : "Agatha Christie" }
                    }
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfUnknownFieldInsideInputs) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
                unknown: "bad field"
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNotScoredPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    pipeOne: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNotScoredPipelineWithFirstPipelineValid) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    pipeOne: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: 5.0 } }
                    ],
                    pipeTwo: [
                        { $match : { age : 50 } }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNotScoredPipelineWithSecondPipelineValid) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    pipeOne: [
                        { $match : { age : 50 } }
                    ],
                    pipeTwo: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$age" } }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfEmptyPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    pipeOne: []
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402503);
}

TEST_F(DocumentSourceScoreFusionTest, CheckMultiplePipelinesAllowed) {
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        StringMap<ResolvedNamespace>{{expCtx->getNamespaceString().coll().toString(),
                                      {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
                }
            },
            inputNormalization: "none"
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
                        "index": "search_index",
                        "text": {
                            "query": "mystery",
                            "path": "genres"
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score",
                                "$name2_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            inputNormalization: "none"
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
                    $search: {
                        index: "search_index",
                        text: {
                            query: "mystery",
                            path: "genres"
                        }
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        StringMap<ResolvedNamespace>{{expCtx->getNamespaceString().coll().toString(),
                                      {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : { $divide: [6.0, 3.0] } } }
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
                }
            },
            inputNormalization: "none"
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
                    $match: { 
                        author : "Agatha Christie"
                    }
                },
                { 
                    $setMetadata: {
                        score: {
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
                                                        { $divide: [ { $const: 6.0 }, { $const: 3.0 } ] }
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
                                $search: {
                                    index: "search_index",
                                    text: {
                                        query: "mystery",
                                        path: "genres"
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
                                    "name3_score": {
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
                        "name3_score": {
                            "$max": {
                                "$ifNull": [
                                    "$name3_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score",
                                "$name2_score",
                                "$name3_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInputNormalizationNotString) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            inputNormalization: 10
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, CheckAnyTypeAllowedForScore) {
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            score: "expression",
            inputNormalization: "none"
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
                    $search: {
                        index: "search_index",
                        text: {
                            query: "mystery",
                            path: "genres"
                        }
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfWeightsIsNotObject) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            score: "expression",
            inputNormalization: "none",
            combination:  {
                weights: "my bad"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfEmptyWeights) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            score: "expression",
            inputNormalization: "none",
            combination: {
                weights: {}
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402202);
}

TEST_F(DocumentSourceScoreFusionTest, CheckIfWeightsArrayMixedIntsDecimals) {
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        StringMap<ResolvedNamespace>{{expCtx->getNamespaceString().coll().toString(),
                                      {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
                }
            },
            score: "expression",
            inputNormalization: "none",
            combination: {
                weights: {
                    name1: 5,
                    name2: 3.2
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
                        "index": "search_index",
                        "text": {
                            "query": "mystery",
                            "path": "genres"
                        }
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score",
                                "$name2_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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

TEST_F(DocumentSourceScoreFusionTest, CheckAnyTypeAllowedForScoreNulls) {
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            score: "expression",
            inputNormalization: "none",
            combination: {
                weights: {
                    name1: 5
                }
            },
            scoreNulls: 0
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
                    $search: {
                        index: "search_index",
                        text: {
                            query: "mystery",
                            path: "genres"
                        }
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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

TEST_F(DocumentSourceScoreFusionTest, ErrorIfOptionalFieldsIncludedMoreThanOnce) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
            },
            score: "expression",
            score: "duplicate",
            inputNormalization: "none",
            combination: {
                weights: {
                    name1: 5
                }
            },
            scoreNulls: 0,
            scoreNulls: 0
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLDuplicateField);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfSearchMetaUsed) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : { $add: [1.0, 4] } } }
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
                        { $score: { score : { $subtract: [4.0, 2] } } }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfSearchStoredSourceUsed) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : 5.0 } }
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
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402501);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfInternalSearchMongotRemoteUsed) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : 5.0 } }
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
                        { $score: { score : 5.0 } }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16436);
}

TEST_F(DocumentSourceScoreFusionTest, CheckLimitSampleUnionwithAllowed) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        StringMap<ResolvedNamespace>{{expCtx->getNamespaceString().coll().toString(),
                                      {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto nsToUnionWith1 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $sample: { size: 10 } },
                        { $score: { score : 5.0 } },
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
                        { $score: { score : 5.0 } }
                    ]
                }
            },
            inputNormalization: "none"
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
                    "$sample": {
                        "size": 10
                    }
                },
                { 
                    $setMetadata: {
                        score: {
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
                                                            $const: 5.0
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
                    "$limit": 10
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
                                "$unionWith": {
                                    "coll": "novels",
                                    "pipeline": [
                                        {
                                            "$limit": 3
                                        },
                                        {
                                            "$unionWith": {
                                                "coll": "shortstories",
                                                "pipeline": []
                                            }
                                        }
                                    ]
                                }
                            },
                            { 
                                $setMetadata: {
                                    score: {
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
                                                                        $const: 5.0
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score",
                                "$name2_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfNestedUnionWithModifiesFields) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        StringMap<ResolvedNamespace>{{expCtx->getNamespaceString().coll().toString(),
                                      {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto nsToUnionWith1 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $sample: { size: 10 } },
                        { $score: { score : 5.0 } },
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
                        { $score: { score : 5.0 } }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), expCtx),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfIncludeProject) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score : 5.0 } }
                    ],
                    name2: [
                        { $match : { age : 50 } },
                        { $score: { score : 5.0 } },
                        { $project: { author: 1 } }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402502);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfPipelineNameDuplicated) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    foo: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: 5.0 } }
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
                        { $score: { score: 5.0 } }
                    ]
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402203);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfPipelineNameStartsWithDollar) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    $matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: {score: 5.0} }
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
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16410);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfPipelineNameContainsDot) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: {score: 5.0} }
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
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16412);
}

TEST_F(DocumentSourceScoreFusionTest, ErrorsIfGeoNearPipeline) {
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
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
                }
            },
            inputNormalization: "none"
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9402500);
}

TEST_F(DocumentSourceScoreFusionTest, CheckIfScoreWithGeoNearDistanceMetadataPipeline) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoringFull", true);
    // Feature flag needed to use 'score' meta field
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $scoreFusion: {
            inputs: {
                pipelines: {
                    name1: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: { $meta: "geoNearDistance" } } }
                    ]
                }
            },
            inputNormalization: "none"
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
                    $match: {
                        author: "Agatha Christie"
                    }
                },
                {
                    $setMetadata: {
                        score: {
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
                                                            $meta: "geoNearDistance"
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$name1_score"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "score": -1,
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
}  // namespace
}  // namespace mongo
