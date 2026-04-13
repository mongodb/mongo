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
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/idl/server_parameter_test_controller.h"
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
    DocumentSourceScoreFusionTest() {}

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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "_internal_scoreFusion_internal_fields.name1_score": -1
                        },
                        "output": {
                            "_internal_scoreFusion_internal_fields.name1_score": {
                                "$minMaxScaler": {
                                    "input": "$_internal_scoreFusion_internal_fields.name1_score",
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "_internal_scoreFusion_internal_fields.name1_score": -1
                        },
                        "output": {
                            "_internal_scoreFusion_internal_fields.name1_score": {
                                "$minMaxScaler": {
                                    "input": "$_internal_scoreFusion_internal_fields.name1_score",
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "_internal_scoreFusion_internal_fields.name2_score": -1
                                    },
                                    "output": {
                                        "_internal_scoreFusion_internal_fields.name2_score": {
                                            "$minMaxScaler": {
                                                "input": "$_internal_scoreFusion_internal_fields.name2_score",
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score",
                                "$_internal_scoreFusion_internal_fields.name2_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score",
                                "$_internal_scoreFusion_internal_fields.name2_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score",
                                "$_internal_scoreFusion_internal_fields.name2_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name3_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name3_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score",
                                        "name3_score": "$__hs_name3_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score",
                                "$_internal_scoreFusion_internal_fields.name2_score",
                                "$_internal_scoreFusion_internal_fields.name3_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchGenres_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchGenres_score": "$__hs_matchGenres_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                "$_internal_scoreFusion_internal_fields.matchGenres_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchGenres_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchGenres_score": "$__hs_matchGenres_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                "$_internal_scoreFusion_internal_fields.matchGenres_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchDistance_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchDistance_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchGenres_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchPlot_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchDistance_score": "$__hs_matchDistance_score",
                                        "matchGenres_score": "$__hs_matchGenres_score",
                                        "matchPlot_score": "$__hs_matchPlot_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                "$_internal_scoreFusion_internal_fields.matchDistance_score",
                                "$_internal_scoreFusion_internal_fields.matchGenres_score",
                                "$_internal_scoreFusion_internal_fields.matchPlot_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name2_score": {
                                            "$multiply": [
                                                {
                                                    "$meta": "score"
                                                },
                                                {
                                                    "$const": 3.2
                                                }
                                            ]
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score",
                                "$_internal_scoreFusion_internal_fields.name2_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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

TEST_F(DocumentSourceScoreFusionTest, QueryShapeDebugString) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$year", normalization: "none" } }
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
            },
            combination: {
                weights: {
                    matchAuthor: 2,
                    matchDistance: 3
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), expCtx);
    const auto pipeline = Pipeline::create(desugaredList, expCtx);

    SerializationOptions opts = SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson(opts));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "HASH<author>": {
                            "$eq": "?string"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$HASH<year>"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "HASH<docs>": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "HASH<internal_raw_score>": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$HASH<docs>"
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "HASH<_internal_scoreFusion_docs>": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "HASH<_internal_scoreFusion_internal_fields>": {
                            "HASH<matchAuthor_score>": {
                                "$multiply": [
                                    {
                                        "$meta": "score"
                                    },
                                    "?number"
                                ]
                            }
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "HASH<pipeline_test>",
                        "pipeline": [
                            {
                                "$geoNear": {
                                    "near": "?object",
                                    "maxDistance": "?number",
                                    "query": {
                                        "HASH<category>": {
                                            "$eq": "?string"
                                        }
                                    },
                                    "spherical": "?bool"
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
                                        "HASH<docs>": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "HASH<internal_raw_score>": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$setMetadata": {
                                    "score": {
                                        "$divide": [
                                            "?number",
                                            {
                                                "$add": [
                                                    "?number",
                                                    {
                                                        "$exp": [
                                                            {
                                                                "$multiply": [
                                                                    "?number",
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
                                    "newRoot": "$HASH<docs>"
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "HASH<_internal_scoreFusion_docs>": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "HASH<_internal_scoreFusion_internal_fields>": {
                                        "HASH<matchDistance_score>": {
                                            "$multiply": [
                                                {
                                                    "$meta": "score"
                                                },
                                                "?number"
                                            ]
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$HASH<_internal_scoreFusion_docs>.HASH<_id>",
                        "HASH<_internal_scoreFusion_docs>": {
                            "$first": "$HASH<_internal_scoreFusion_docs>"
                        },
                        "HASH<__hs_matchAuthor_score>": {
                            "$max": {
                                "$ifNull": [
                                    "$HASH<_internal_scoreFusion_internal_fields>.HASH<matchAuthor_score>",
                                    "?number"
                                ]
                            }
                        },
                        "HASH<__hs_matchDistance_score>": {
                            "$max": {
                                "$ifNull": [
                                    "$HASH<_internal_scoreFusion_internal_fields>.HASH<matchDistance_score>",
                                    "?number"
                                ]
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$HASH<_internal_scoreFusion_docs>",
                                {
                                    "HASH<_internal_scoreFusion_internal_fields>": {
                                        "HASH<matchAuthor_score>": "$HASH<__hs_matchAuthor_score>",
                                        "HASH<matchDistance_score>": "$HASH<__hs_matchDistance_score>"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$HASH<_internal_scoreFusion_internal_fields>.HASH<matchAuthor_score>",
                                "$HASH<_internal_scoreFusion_internal_fields>.HASH<matchDistance_score>"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "$computed0": {
                            "$meta": "score"
                        },
                        "HASH<_id>": 1
                    }
                },
                {
                    "$project": {
                        "HASH<_internal_scoreFusion_internal_fields>": false,
                        "HASH<_id>": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, RepresentativeQueryShape) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $score: { score: "$year", normalization: "none" } }
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
            },
            combination: {
                weights: {
                    matchAuthor: 2,
                    matchDistance: 3
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), expCtx);
    const auto pipeline = Pipeline::create(desugaredList, expCtx);

    SerializationOptions opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson(opts));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "author": {
                            "$eq": "?"
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": "$year"
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "matchAuthor_score": {
                                "$multiply": [
                                    {
                                        "$meta": "score"
                                    },
                                    1
                                ]
                            }
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$geoNear": {
                                    "near": {
                                        "$const": {
                                            "?": "?"
                                        }
                                    },
                                    "maxDistance": 1,
                                    "query": {
                                        "category": {
                                            "$eq": "?"
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
                                            1,
                                            {
                                                "$add": [
                                                    1,
                                                    {
                                                        "$exp": [
                                                            {
                                                                "$multiply": [
                                                                    1,
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "matchDistance_score": {
                                            "$multiply": [
                                                {
                                                    "$meta": "score"
                                                },
                                                1
                                            ]
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                    1
                                ]
                            }
                        },
                        "__hs_matchDistance_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchDistance_score",
                                    1
                                ]
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchDistance_score": "$__hs_matchDistance_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.matchAuthor_score",
                                "$_internal_scoreFusion_internal_fields.matchDistance_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);  // NOLINT

    // Ensure the representative query shape is reparseable.
    ASSERT_DOES_NOT_THROW(pipeline_factory::makePipeline(
        asOneObj.firstElement(), expCtx, {.attachCursorSource = false}));
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_matchDistance_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchDistance_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchGenres_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "matchDistance_score": "$__hs_matchDistance_score",
                                        "matchGenres_score": "$__hs_matchGenres_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.matchDistance_score",
                                "$_internal_scoreFusion_internal_fields.matchGenres_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score",
                                "$_internal_scoreFusion_internal_fields.name2_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$_internal_scoreFusion_internal_fields.name1_score",
                                    "name2": "$_internal_scoreFusion_internal_fields.name2_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name2_score": "$__hs_name2_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$_internal_scoreFusion_internal_fields.name1_score",
                                    "name2": "$_internal_scoreFusion_internal_fields.name2_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$_internal_scoreFusion_internal_fields.name1_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceScoreFusionTest, CheckOnePipelineVectorSearchScoreDetailsDesugaring) {
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "name1_rawScore": {
                                "$meta": "score"
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "name1_scoreDetails": {
                                "details": []
                            }
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name1_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name1_rawScore": "$__hs_name1_rawScore",
                                        "name1_scoreDetails": "$__hs_name1_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "name1"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                            "weight": {
                                                "$const": 5
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.name1_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                                    ]
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
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "name1_rawScore": {
                                "$meta": "score"
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "name1_scoreDetails": {
                                "details": []
                            }
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "searchPipe_rawScore": {
                                            "$meta": "score"
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "searchPipe_scoreDetails": {
                                            "details": {
                                                "$meta": "scoreDetails"
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name1_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                        },
                        "__hs_searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.searchPipe_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.searchPipe_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.searchPipe_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name1_rawScore": "$__hs_name1_rawScore",
                                        "name1_scoreDetails": "$__hs_name1_scoreDetails",
                                        "searchPipe_score": "$__hs_searchPipe_score",
                                        "searchPipe_rawScore": "$__hs_searchPipe_rawScore",
                                        "searchPipe_scoreDetails": "$__hs_searchPipe_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.name1_score",
                                "$_internal_scoreFusion_internal_fields.searchPipe_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "name1"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                            "weight": {
                                                "$const": 3
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.name1_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "searchPipe"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.searchPipe_rawScore",
                                            "weight": {
                                                "$const": 2
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.searchPipe_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.searchPipe_scoreDetails"
                                    ]
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
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "search_rawScore": {
                                "$meta": "score"
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "search_scoreDetails": {
                                "details": []
                            }
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "vector_rawScore": {
                                            "$meta": "score"
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "vector_scoreDetails": {
                                            "details": []
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_search_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.search_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_search_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.search_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_search_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.search_scoreDetails"
                        },
                        "__hs_vector_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.vector_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_vector_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.vector_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_vector_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.vector_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "search_score": "$__hs_search_score",
                                        "search_rawScore": "$__hs_search_rawScore",
                                        "search_scoreDetails": "$__hs_search_scoreDetails",
                                        "vector_score": "$__hs_vector_score",
                                        "vector_rawScore": "$__hs_vector_rawScore",
                                        "vector_scoreDetails": "$__hs_vector_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.search_score",
                                "$_internal_scoreFusion_internal_fields.vector_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "search"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.search_rawScore",
                                            "weight": {
                                                "$const": 2
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.search_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.search_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "vector"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.vector_rawScore",
                                            "weight": {
                                                "$const": 1
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.vector_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.vector_scoreDetails"
                                    ]
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
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "name1_rawScore": {
                                "$meta": "score"
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "name1_scoreDetails": {
                                "details": []
                            }
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "searchPipe_rawScore": {
                                            "$meta": "score"
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "searchPipe_scoreDetails": {
                                            "details": {
                                                "$meta": "scoreDetails"
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_name1_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                        },
                        "__hs_searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.searchPipe_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.searchPipe_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.searchPipe_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "name1_score": "$__hs_name1_score",
                                        "name1_rawScore": "$__hs_name1_rawScore",
                                        "name1_scoreDetails": "$__hs_name1_scoreDetails",
                                        "searchPipe_score": "$__hs_searchPipe_score",
                                        "searchPipe_rawScore": "$__hs_searchPipe_rawScore",
                                        "searchPipe_scoreDetails": "$__hs_searchPipe_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "name1": "$_internal_scoreFusion_internal_fields.name1_score",
                                    "searchPipe": "$_internal_scoreFusion_internal_fields.searchPipe_score"
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
                        "_internal_scoreFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "name1"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                            "weight": {
                                                "$const": 1
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.name1_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "searchPipe"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.searchPipe_rawScore",
                                            "weight": {
                                                "$const": 1
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.searchPipe_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.searchPipe_scoreDetails"
                                    ]
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
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
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

    auto expected = std::string(R"({
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "scorePipe1_rawScore": {
                                "$meta": "score"
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "scorePipe1_scoreDetails": {
                                "details": []
                            }
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "_internal_scoreFusion_internal_fields.scorePipe1_score": -1
                        },
                        "output": {
                            "_internal_scoreFusion_internal_fields.scorePipe1_score": {
                                "$minMaxScaler": {
                                    "input": "$_internal_scoreFusion_internal_fields.scorePipe1_score",
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "scorePipe2_rawScore": {
                                            "$meta": "score"
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "scorePipe2_scoreDetails": {
                                            "details": []
                                        }
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "_internal_scoreFusion_internal_fields.scorePipe2_score": -1
                                    },
                                    "output": {
                                        "_internal_scoreFusion_internal_fields.scorePipe2_score": {
                                            "$minMaxScaler": {
                                                "input": "$_internal_scoreFusion_internal_fields.scorePipe2_score",
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
)") + R"(                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe1_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.scorePipe1_scoreDetails"
                        },
                        "__hs_scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe2_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "scorePipe1_score": "$__hs_scorePipe1_score",
                                        "scorePipe1_rawScore": "$__hs_scorePipe1_rawScore",
                                        "scorePipe1_scoreDetails": "$__hs_scorePipe1_scoreDetails",
                                        "scorePipe2_score": "$__hs_scorePipe2_score",
                                        "scorePipe2_rawScore": "$__hs_scorePipe2_rawScore",
                                        "scorePipe2_scoreDetails": "$__hs_scorePipe2_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "scorePipe1": "$_internal_scoreFusion_internal_fields.scorePipe1_score",
                                    "scorePipe2": "$_internal_scoreFusion_internal_fields.scorePipe2_score"
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
                        "_internal_scoreFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "scorePipe1"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.scorePipe1_rawScore",
                                            "weight": {
                                                "$const": 1
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.scorePipe1_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.scorePipe1_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "scorePipe2"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.scorePipe2_rawScore",
                                            "weight": {
                                                "$const": 1
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.scorePipe2_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.scorePipe2_scoreDetails"
                                    ]
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
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        }
)";  // NOLINT
    ASSERT_BSONOBJ_EQ(fromjson(expected), asOneObj);
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

    auto expected = std::string(R"({
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "scorePipe1_rawScore": {
                                "$meta": "score"
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
                            "scorePipe1_scoreDetails": {
                                "details": {
                                    "$meta": "scoreDetails"
                                }
                            }
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "_internal_scoreFusion_internal_fields.scorePipe1_score": -1
                        },
                        "output": {
                            "_internal_scoreFusion_internal_fields.scorePipe1_score": {
                                "$minMaxScaler": {
                                    "input": "$_internal_scoreFusion_internal_fields.scorePipe1_score",
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "scorePipe2_rawScore": {
                                            "$meta": "score"
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
                                        "scorePipe2_scoreDetails": {
                                            "details": {
                                                "$meta": "scoreDetails"
                                            }
                                        }
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "_internal_scoreFusion_internal_fields.scorePipe2_score": -1
                                    },
                                    "output": {
                                        "_internal_scoreFusion_internal_fields.scorePipe2_score": {
                                            "$minMaxScaler": {
                                                "input": "$_internal_scoreFusion_internal_fields.scorePipe2_score",
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
)") + R"(                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_scorePipe1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe1_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe1_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe1_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.scorePipe1_scoreDetails"
                        },
                        "__hs_scorePipe2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe2_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scorePipe2_rawScore",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_scorePipe2_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.scorePipe2_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "scorePipe1_score": "$__hs_scorePipe1_score",
                                        "scorePipe1_rawScore": "$__hs_scorePipe1_rawScore",
                                        "scorePipe1_scoreDetails": "$__hs_scorePipe1_scoreDetails",
                                        "scorePipe2_score": "$__hs_scorePipe2_score",
                                        "scorePipe2_rawScore": "$__hs_scorePipe2_rawScore",
                                        "scorePipe2_scoreDetails": "$__hs_scorePipe2_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$let": {
                                "vars": {
                                    "scorePipe1": "$_internal_scoreFusion_internal_fields.scorePipe1_score",
                                    "scorePipe2": "$_internal_scoreFusion_internal_fields.scorePipe2_score"
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
                        "_internal_scoreFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "scorePipe1"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.scorePipe1_rawScore",
                                            "weight": {
                                                "$const": 1
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.scorePipe1_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.scorePipe1_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "scorePipe2"
                                            },
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.scorePipe2_rawScore",
                                            "weight": {
                                                "$const": 1
                                            },
                                            "value": "$_internal_scoreFusion_internal_fields.scorePipe2_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.scorePipe2_scoreDetails"
                                    ]
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
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        }
)";  // NOLINT
    ASSERT_BSONOBJ_EQ(fromjson(expected), asOneObj);
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

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "_internal_scoreFusion_internal_fields.double_score": -1
                        },
                        "output": {
                            "_internal_scoreFusion_internal_fields.double_score": {
                                "$minMaxScaler": {
                                    "input": "$_internal_scoreFusion_internal_fields.double_score",
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "_internal_scoreFusion_internal_fields.single_score": -1
                                    },
                                    "output": {
                                        "_internal_scoreFusion_internal_fields.single_score": {
                                            "$minMaxScaler": {
                                                "input": "$_internal_scoreFusion_internal_fields.single_score",
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_double_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.double_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_single_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.single_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "double_score": "$__hs_double_score",
                                        "single_score": "$__hs_single_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.double_score",
                                "$_internal_scoreFusion_internal_fields.single_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
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

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
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
                            "_internal_scoreFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields": {
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
                    }
                },
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
                                        "_internal_scoreFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields": {
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_double_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.double_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_single_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.single_score",
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
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "double_score": "$__hs_double_score",
                                        "single_score": "$__hs_single_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$avg": [
                                "$_internal_scoreFusion_internal_fields.double_score",
                                "$_internal_scoreFusion_internal_fields.single_score"
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
                    "$project": {
                        "_internal_scoreFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

// This test demonstrates what happens to user fields that match our internal names as it goes
// through the process to get score details.
TEST_F(DocumentSourceScoreFusionTest, InternalFieldBehaviorThroughGroupAndReshape) {
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

    // Build the desugared pipeline to extract the $group and subsequent stages.
    const auto desugaredList =
        DocumentSourceScoreFusion::createFromBson(spec.firstElement(), getExpCtx());

    // Find the $group stage and collect it plus all following stages.
    std::vector<boost::intrusive_ptr<DocumentSource>> postGroupStages;
    bool foundGroup = false;
    for (const auto& stage : desugaredList) {
        if (foundGroup) {
            postGroupStages.push_back(stage);
        } else {
            auto serialized = stage->serialize();
            auto stageDoc = serialized.getDocument();
            if (!stageDoc["$group"_sd].missing()) {
                foundGroup = true;
                postGroupStages.push_back(stage);
            }
        }
    }
    ASSERT_TRUE(foundGroup);

    // Mock documents simulating the state entering the $group stage. Each document represents a
    // single input pipeline's contribution for a given user document. The user document is wrapped
    // in _internal_scoreFusion_docs and includes:
    //   - a field with the __hs_ prefix that will survive the pipeline,
    //   - a field named "_internal_scoreFusion_internal_fields" that collides with the internal
    //     name and will be lost.
    auto mock = exec::agg::MockStage::createForTest(
        {
            Document{{"_internal_scoreFusion_docs",
                      Document{{"_id", 1},
                               {"val", 10},
                               {"__hs_custom", "alpha"_sd},
                               {"_internal_scoreFusion_internal_fields", "user_data"_sd}}},
                     {"_internal_scoreFusion_internal_fields", Document{{"name1_score", 3.0}}}},
            Document{{"_internal_scoreFusion_docs",
                      Document{{"_id", 2}, {"val", 20}, {"__hs_custom", "beta"_sd}}},
                     {"_internal_scoreFusion_internal_fields", Document{{"name1_score", 7.0}}}},
        },
        getExpCtx());

    // Build exec stages from the DocumentSource list and chain them together with the mock source.
    // Keep all intermediate stages alive to prevent use-after-free.
    std::vector<boost::intrusive_ptr<exec::agg::Stage>> execStages;
    execStages.push_back(mock);
    for (auto& ds : postGroupStages) {
        execStages.push_back(exec::agg::buildStageAndStitch(ds, execStages.back()));
    }
    auto& lastStage = execStages.back();

    // Execute and collect results from the last stage.
    std::vector<Document> results;
    for (auto next = lastStage->getNext(); next.isAdvanced(); next = lastStage->getNext()) {
        results.push_back(next.releaseDocument());
    }

    ASSERT_EQ(results.size(), 2u);

    // Results are sorted by score descending then _id ascending, so doc with score 7 comes first.
    ASSERT_VALUE_EQ(results[0]["_id"_sd], Value(2));
    ASSERT_VALUE_EQ(results[1]["_id"_sd], Value(1));

    // Verify user fields with the __hs_ prefix are preserved.
    ASSERT_VALUE_EQ(results[0]["__hs_custom"_sd], Value("beta"_sd));
    ASSERT_VALUE_EQ(results[1]["__hs_custom"_sd], Value("alpha"_sd));

    // Verify other user fields are preserved.
    ASSERT_VALUE_EQ(results[0]["val"_sd], Value(20));
    ASSERT_VALUE_EQ(results[1]["val"_sd], Value(10));

    // A user field named "_internal_scoreFusion_internal_fields" is lost: $mergeObjects overwrites
    // it with the repacked internal fields object, and then $project removes it.
    ASSERT_TRUE(results[0]["_internal_scoreFusion_internal_fields"_sd].missing());
    ASSERT_TRUE(results[1]["_internal_scoreFusion_internal_fields"_sd].missing());

    // All internal fields are removed from the output.
    ASSERT_TRUE(results[0]["_internal_scoreFusion_docs"_sd].missing());
    ASSERT_TRUE(results[1]["_internal_scoreFusion_docs"_sd].missing());
    ASSERT_TRUE(results[0]["__hs_name1_score"_sd].missing());
    ASSERT_TRUE(results[1]["__hs_name1_score"_sd].missing());
}
}  // namespace
}  // namespace mongo
