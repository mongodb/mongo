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

#include "mongo/db/pipeline/document_source_rank_fusion.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include "expression_context.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
class DocumentSourceRankFusionTest : service_context_test::WithSetupTransportLayer,
                                     public AggregationContextFixture {
private:
    RAIIServerParameterControllerForTest featureFlagController1{"featureFlagRankFusionBasic", true};
    RAIIServerParameterControllerForTest featureFlagController2{"featureFlagRankFusionFull", true};
};

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNoInputsField) {
    auto spec = fromjson(R"({
        $rankFusion: {
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNoNestedObject) {
    auto spec = fromjson(R"({
        $rankFusion: 'not_an_object'
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::FailedToParse);
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

TEST_F(DocumentSourceRankFusionTest, ErrorsIfInputsIsNotObject) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {pipelines: "not an object"}
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNoPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {}
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::BadValue);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfMissingPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {}
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

TEST_F(DocumentSourceRankFusionTest, CheckOnePipelineAllowedBasicRankFusion) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", false);
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                                    {
                                                        "$const": 60
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
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "agatha_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "agatha_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "agatha_score": {
                                        "$max": [
                                            "$$value.agatha_score",
                                            "$$this.agatha_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.agatha_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score"
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
                    "$project": {
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfPipelineIsNotArray) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    authorMatch: {
                        $match : { author : "Agatha Christie" }
                    }
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfUnknownFieldInsideInputs) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    authorMatch: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                },
                unknown: "bad field"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}


TEST_F(DocumentSourceRankFusionTest, ErrorsIfNotRankedPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    pipeOne: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    pipeTwo: [
                        { $match : { age : 50 } }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191100);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNestedRankFusionPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
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
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10473002);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfScoreStageInInputPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $score: {
                            score: 10
                        } }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10614800);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNestedScoreFusionPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
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
                                }
                            }
                        } }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       10473002);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfEmptyPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    pipeOne: []
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9834300);
}

TEST_F(DocumentSourceRankFusionTest,
       CheckMultiplePipelinesAndOptionalArgumentsAllowedBasicRankFusion) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", false);

    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
                    ]
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    // The expected desugar is too large for the compiler so we need to split it up.
    const std::string expectedStages = std::string(R"({
            "expectedStages": [)") +
        std::string(R"(
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                                    {
                                                        "$const": 60
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
                },)") +
        std::string(R"(
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchGenres_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                                                {
                                                                    "$const": 60
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
                },)") +
        std::string(R"(
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchPlot_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchPlot_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchPlot_rank",
                                                                {
                                                                    "$const": 60
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
                },)") +
        std::string(R"(
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "matchAuthor_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchGenres_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchGenres_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchPlot_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchPlot_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "matchAuthor_score": {
                                        "$const": 0
                                    },
                                    "matchGenres_score": {
                                        "$const": 0
                                    },
                                    "matchPlot_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "matchAuthor_score": {
                                        "$max": [
                                            "$$value.matchAuthor_score",
                                            "$$this.matchAuthor_score"
                                        ]
                                    },
                                    "matchGenres_score": {
                                        "$max": [
                                            "$$value.matchGenres_score",
                                            "$$this.matchGenres_score"
                                        ]
                                    },
                                    "matchPlot_score": {
                                        "$max": [
                                            "$$value.matchPlot_score",
                                            "$$this.matchPlot_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank"
                                ]
                            },
                            "matchGenres_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchGenres_rank"
                                ]
                            },
                            "matchPlot_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchPlot_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchPlot_rank"
                                ]
                            }
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                "$_internal_rankFusion_internal_fields.matchGenres_score",
                                "$_internal_rankFusion_internal_fields.matchPlot_score"
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
                    "$project": {
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ_AUTO(expectedStages, asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfSearchMetaUsed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    matchGenre: [
                        {
                            $searchMeta: {
                                index: "search_index",
                                text: {
                                    query: "mystery",
                                    path: "genres"
                                }
                            }
                        },
                        { $sort: {genres: 1} }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfSearchStoredSourceUsed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    matchGenre: [
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
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfInternalSearchMongotRemoteUsed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    matchGenre: [
                        {
                            $_internalSearchMongotRemote: {
                                index: "search_index",
                                text: {
                                    query: "mystery",
                                    path: "genres"
                                }
                            }
                        },
                        { $sort: {genres: 1} }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, CheckLimitSampleUnionwithNotAllowed) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto nsToUnionWith1 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    sample: [
                        { $sample: { size: 10 } },
                        { $sort: {author: 1} },
                        { $limit: 10 }
                    ],
                    unionWith: [
                        { $unionWith: {
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
                        { $sort: {author: 1} }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), expCtx),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNestedUnionWithModifiesFields) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto nsToUnionWith1 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 = NamespaceString::createNamespaceString_forTest(
        expCtx->getNamespaceString().dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    sample: [
                        { $sample: { size: 10 } },
                        { $sort: {author: 1} },
                        { $limit: 10 }
                    ],
                    unionWith: [
                        { $unionWith: {
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
                        { $sort: {author: 1} }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), expCtx),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, CheckGeoNearAllowedWhenNoIncludeLocsAndNoDistanceField) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    geo: [
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
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                                    {
                                                        "$const": 60
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
                                "$replaceRoot": {
                                    "newRoot": {
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.geo_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "geo_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.geo_rank",
                                                                {
                                                                    "$const": 60
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
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "agatha_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "geo_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.geo_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "agatha_score": {
                                        "$const": 0
                                    },
                                    "geo_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "agatha_score": {
                                        "$max": [
                                            "$$value.agatha_score",
                                            "$$this.agatha_score"
                                        ]
                                    },
                                    "geo_score": {
                                        "$max": [
                                            "$$value.geo_score",
                                            "$$this.geo_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.agatha_rank"
                                ]
                            },
                            "geo_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.geo_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.geo_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score",
                                "$_internal_rankFusion_internal_fields.geo_score"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfGeoNearIncludeLocs) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    geo: [
                        {
                            $geoNear: {
                                near: { type: "Point", coordinates: [ -73.99279 , 40.719296 ] },
                                maxDistance: 2,
                                query: { category: "Parks" },
                                includeLocs: "dist.location",
                                spherical: true
                            }
                        }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfGeoNearDistanceField) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    geo: [
                        {
                            $geoNear: {
                                near: { type: "Point", coordinates: [ -73.99279 , 40.719296 ] },
                                distanceField: "dist.calculated",
                                maxDistance: 2,
                                query: { category: "Parks" },
                                spherical: true
                            }
                        }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfIncludeProject) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    elderAuthors: [
                        { $match : { age : 50 } },
                        { $sort: {author: 1} },
                        { $project: { author: 1 } }
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfCombinationIsNotObject) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: 5
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}


TEST_F(DocumentSourceRankFusionTest, ErrorsIfCombinationHasUnknownField) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                invalidField: 5
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLUnknownField);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfWeightsIsNotObject) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination:  {
                weights: "my bad"
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceRankFusionTest, DoesNotErrorIfEmptyWeights) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {}
            }
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceRankFusionTest, DoesNotErrorIfOnlySomeWeights) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {
                    matchAuthor: 3
                }
            }
        }
    })");

    ASSERT_DOES_NOT_THROW(
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfMisnamedWeight) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {
                    matchAuthor: 3,
                    matchGenre: 2
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9967500);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfExtraWeight) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {
                    matchAuthor: 3,
                    matchGenre: 2,
                    matchGenres: 1
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9460301);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNonNumericWeight) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {
                    matchAuthor: 3,
                    matchGenres: "0"
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       13118);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNegativeWeightValue) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {
                    matchAuthor: -1,
                    matchGenres: 0
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9460300);
}

TEST_F(DocumentSourceRankFusionTest, CheckWeightsApplied) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {
                    matchAuthor: 5,
                    matchGenres: 3
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
                                            }
                                        ]
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchGenres_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                                                {
                                                                    "$const": 60
                                                                }
                                                            ]
                                                        }
                                                    ]
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
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "matchAuthor_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchGenres_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchGenres_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "matchAuthor_score": {
                                        "$const": 0
                                    },
                                    "matchGenres_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "matchAuthor_score": {
                                        "$max": [
                                            "$$value.matchAuthor_score",
                                            "$$this.matchAuthor_score"
                                        ]
                                    },
                                    "matchGenres_score": {
                                        "$max": [
                                            "$$value.matchGenres_score",
                                            "$$this.matchGenres_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank"
                                ]
                            },
                            "matchGenres_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchGenres_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                "$_internal_rankFusion_internal_fields.matchGenres_score"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

// Same as CheckWeightsApplied but the ordering of fields doesn't match between input.pipelines and
// combination.weights; checks that the weights are applied to the pipeline with the same name.
TEST_F(DocumentSourceRankFusionTest, CheckWeightsAppliedToCorrectPipeline) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            combination: {
                weights: {
                    matchGenres: 3,
                    matchAuthor: 5
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
                                            }
                                        ]
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchGenres_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                                                {
                                                                    "$const": 60
                                                                }
                                                            ]
                                                        }
                                                    ]
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
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "matchAuthor_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchGenres_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchGenres_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "matchAuthor_score": {
                                        "$const": 0
                                    },
                                    "matchGenres_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "matchAuthor_score": {
                                        "$max": [
                                            "$$value.matchAuthor_score",
                                            "$$this.matchAuthor_score"
                                        ]
                                    },
                                    "matchGenres_score": {
                                        "$max": [
                                            "$$value.matchGenres_score",
                                            "$$this.matchGenres_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank"
                                ]
                            },
                            "matchGenres_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchGenres_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                "$_internal_rankFusion_internal_fields.matchGenres_score"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, CheckWeightsAppliedMultiplePipelines) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    // The expected desugar is too large for the compiler so we need to split it up.
    const std::string expectedStages = std::string(R"({
            "expectedStages": [)") +
        std::string(R"(
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
                                            }
                                        ]
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
                                "$replaceRoot": {
                                    "newRoot": {
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchDistance_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchDistance_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchDistance_rank",
                                                                {
                                                                    "$const": 60
                                                                }
                                                            ]
                                                        }
                                                    ]
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
                },)") +
        std::string(R"(
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchGenres_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                                                {
                                                                    "$const": 60
                                                                }
                                                            ]
                                                        }
                                                    ]
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchPlot_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchPlot_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchPlot_rank",
                                                                {
                                                                    "$const": 60
                                                                }
                                                            ]
                                                        }
                                                    ]
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
                },)") +
        std::string(R"(
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "matchAuthor_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchDistance_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchDistance_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchGenres_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchGenres_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchPlot_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchPlot_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "matchAuthor_score": {
                                        "$const": 0
                                    },
                                    "matchDistance_score": {
                                        "$const": 0
                                    },
                                    "matchGenres_score": {
                                        "$const": 0
                                    },
                                    "matchPlot_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "matchAuthor_score": {
                                        "$max": [
                                            "$$value.matchAuthor_score",
                                            "$$this.matchAuthor_score"
                                        ]
                                    },
                                    "matchDistance_score": {
                                        "$max": [
                                            "$$value.matchDistance_score",
                                            "$$this.matchDistance_score"
                                        ]
                                    },
                                    "matchGenres_score": {
                                        "$max": [
                                            "$$value.matchGenres_score",
                                            "$$this.matchGenres_score"
                                        ]
                                    },
                                    "matchPlot_score": {
                                        "$max": [
                                            "$$value.matchPlot_score",
                                            "$$this.matchPlot_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank"
                                ]
                            },
                            "matchDistance_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchDistance_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchDistance_rank"
                                ]
                            },
                            "matchGenres_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchGenres_rank"
                                ]
                            },
                            "matchPlot_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchPlot_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchPlot_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                "$_internal_rankFusion_internal_fields.matchDistance_score",
                                "$_internal_rankFusion_internal_fields.matchGenres_score",
                                "$_internal_rankFusion_internal_fields.matchPlot_score"
                            ]
                        }
                    }
                },)") +
        std::string(R"(
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ_AUTO(expectedStages, asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, ScoreDetailsIsRejectedWithoutRankFusionFullFF) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", false);
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                }
            },
            combination: {
                weights: {
                    agatha: 5
                }
            },
            scoreDetails: true
        }
    })");
    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::QueryFeatureNotAllowed);
}

TEST_F(DocumentSourceRankFusionTest, CheckOnePipelineScoreDetailsDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                }
            },
            combination: {
                weights: {
                    agatha: 5
                }
            },
            scoreDetails: true
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
                                            }
                                        ]
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
                        "_internal_rankFusion_internal_fields": {
                            "agatha_scoreDetails": {
                                "details": []
                            }
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "agatha_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "agatha_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "agatha_scoreDetails": "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "agatha_score": {
                                        "$const": 0
                                    },
                                    "agatha_rank": {
                                        "$const": 0
                                    },
                                    "agatha_scoreDetails": {
                                        "$const": {}
                                    }
                                },
                                "in": {
                                    "agatha_score": {
                                        "$max": [
                                            "$$value.agatha_score",
                                            "$$this.agatha_score"
                                        ]
                                    },
                                    "agatha_rank": {
                                        "$max": [
                                            "$$value.agatha_rank",
                                            "$$this.agatha_rank"
                                        ]
                                    },
                                    "agatha_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.agatha_scoreDetails",
                                            "$$this.agatha_scoreDetails"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.agatha_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "agatha"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.agatha_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 5
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
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
                                "$const": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:"
                            },
                            "details": "$_internal_rankFusion_internal_fields.calculatedScoreDetails"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, CheckOneScorePipelineScoreDetailsDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
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
            combination: {
                weights: {
                    agatha: 5
                }
            },
            scoreDetails: true
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
                                            }
                                        ]
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
                        "_internal_rankFusion_internal_fields": {
                            "agatha_scoreDetails": {
                                "value": {
                                    "$meta": "score"
                                },
                                "details": []
                            }
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "agatha_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "agatha_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "agatha_scoreDetails": "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "agatha_score": {
                                        "$const": 0
                                    },
                                    "agatha_rank": {
                                        "$const": 0
                                    },
                                    "agatha_scoreDetails": {
                                        "$const": {}
                                    }
                                },
                                "in": {
                                    "agatha_score": {
                                        "$max": [
                                            "$$value.agatha_score",
                                            "$$this.agatha_score"
                                        ]
                                    },
                                    "agatha_rank": {
                                        "$max": [
                                            "$$value.agatha_rank",
                                            "$$this.agatha_rank"
                                        ]
                                    },
                                    "agatha_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.agatha_scoreDetails",
                                            "$$this.agatha_scoreDetails"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.agatha_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "agatha"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.agatha_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 5
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
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
                                "$const": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:"
                            },
                            "details": "$_internal_rankFusion_internal_fields.calculatedScoreDetails"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, CheckTwoPipelineScoreDetailsDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
                }
            },
            combination: {
                weights: {
                    searchPipe: 2
                }
            },
            scoreDetails: true
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    // The expected desugar is too large for the compiler so we need to split it up.
    const std::string expectedStages = std::string(R"({
            "expectedStages": [)") +
        std::string(R"(
                {
                    "$match": {
                        "author": "Agatha Christie"
                    }
                },
                {
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                                    {
                                                        "$const": 60
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
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_scoreDetails": {
                                "details": []
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.searchPipe_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "searchPipe_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                                                {
                                                                    "$const": 60
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
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "searchPipe_scoreDetails": {
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
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "agatha_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "agatha_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "agatha_scoreDetails": "$_internal_rankFusion_internal_fields.agatha_scoreDetails",
                                "searchPipe_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.searchPipe_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "searchPipe_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "searchPipe_scoreDetails": "$_internal_rankFusion_internal_fields.searchPipe_scoreDetails"
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "agatha_score": {
                                        "$const": 0
                                    },
                                    "agatha_rank": {
                                        "$const": 0
                                    },
                                    "agatha_scoreDetails": {
                                        "$const": {}
                                    },
                                    "searchPipe_score": {
                                        "$const": 0
                                    },
                                    "searchPipe_rank": {
                                        "$const": 0
                                    },
                                    "searchPipe_scoreDetails": {
                                        "$const": {}
                                    }
                                },
                                "in": {
                                    "agatha_score": {
                                        "$max": [
                                            "$$value.agatha_score",
                                            "$$this.agatha_score"
                                        ]
                                    },
                                    "agatha_rank": {
                                        "$max": [
                                            "$$value.agatha_rank",
                                            "$$this.agatha_rank"
                                        ]
                                    },
                                    "agatha_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.agatha_scoreDetails",
                                            "$$this.agatha_scoreDetails"
                                        ]
                                    },
                                    "searchPipe_score": {
                                        "$max": [
                                            "$$value.searchPipe_score",
                                            "$$this.searchPipe_score"
                                        ]
                                    },
                                    "searchPipe_rank": {
                                        "$max": [
                                            "$$value.searchPipe_rank",
                                            "$$this.searchPipe_rank"
                                        ]
                                    },
                                    "searchPipe_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.searchPipe_scoreDetails",
                                            "$$this.searchPipe_scoreDetails"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.agatha_rank"
                                ]
                            },
                            "searchPipe_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.searchPipe_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score",
                                "$_internal_rankFusion_internal_fields.searchPipe_score"
                            ]
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "agatha"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.agatha_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 1
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "searchPipe"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 2
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.searchPipe_scoreDetails"
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
                                "$const": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:"
                            },
                            "details": "$_internal_rankFusion_internal_fields.calculatedScoreDetails"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ_AUTO(expectedStages, asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfPipelineNameEmpty) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    "": [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       15998);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfPipelineNameDuplicated) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    foo: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
                        {$sort: {author: 1}}
                    ]
                }
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9921000);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfPipelineNameStartsWithDollar) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    $matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16410);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfPipelineNameContainsDot) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16412);
}

TEST_F(DocumentSourceRankFusionTest, QueryShapeDebugString) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    matchDistance: [
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
            combination: {
                weights: {
                    matchAuthor: 2,
                    matchDistance: 3
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), expCtx);
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
                    "$sort": {
                        "HASH<author>": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "HASH<_internal_rankFusion_docs>": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "HASH<order>": 1
                        },
                        "output": {
                            "HASH<_internal_rankFusion_internal_fields>.HASH<matchAuthor_rank>": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "HASH<_internal_rankFusion_internal_fields>": {
                            "HASH<matchAuthor_score>": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            "?number",
                                            {
                                                "$add": [
                                                    "$HASH<_internal_rankFusion_internal_fields>.HASH<matchAuthor_rank>",
                                                    "?number"
                                                ]
                                            }
                                        ]
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
                                "$replaceRoot": {
                                    "newRoot": {
                                        "HASH<_internal_rankFusion_docs>": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "HASH<order>": 1
                                    },
                                    "output": {
                                        "HASH<_internal_rankFusion_internal_fields>.HASH<matchDistance_rank>": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "HASH<_internal_rankFusion_internal_fields>": {
                                        "HASH<matchDistance_score>": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        "?number",
                                                        {
                                                            "$add": [
                                                                "$HASH<_internal_rankFusion_internal_fields>.HASH<matchDistance_rank>",
                                                                "?number"
                                                            ]
                                                        }
                                                    ]
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
                        "_id": "$HASH<_internal_rankFusion_docs>.HASH<_id>",
                        "HASH<_internal_rankFusion_docs>": {
                            "$first": "$HASH<_internal_rankFusion_docs>"
                        },
                        "HASH<_internal_rankFusion_internal_fields>": {
                            "$push": {
                                "HASH<matchAuthor_score>": {
                                    "$ifNull": [
                                        "$HASH<_internal_rankFusion_internal_fields>.HASH<matchAuthor_score>",
                                        "?number"
                                    ]
                                },
                                "HASH<matchDistance_score>": {
                                    "$ifNull": [
                                        "$HASH<_internal_rankFusion_internal_fields>.HASH<matchDistance_score>",
                                        "?number"
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$project": {
                        "HASH<_id>": true,
                        "HASH<_internal_rankFusion_docs>": true,
                        "HASH<_internal_rankFusion_internal_fields>": {
                            "$reduce": {
                                "input": "$HASH<_internal_rankFusion_internal_fields>",
                                "initialValue": "?object",
                                "in": {
                                    "HASH<matchAuthor_score>": {
                                        "$max": [
                                            "$$HASH<value>.HASH<matchAuthor_score>",
                                            "$$HASH<this>.HASH<matchAuthor_score>"
                                        ]
                                    },
                                    "HASH<matchDistance_score>": {
                                        "$max": [
                                            "$$HASH<value>.HASH<matchDistance_score>",
                                            "$$HASH<this>.HASH<matchDistance_score>"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$HASH<_internal_rankFusion_docs>",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "HASH<_internal_rankFusion_docs>": false,
                        "HASH<_id>": true
                    }
                },
                {
                    "$addFields": {
                        "HASH<_internal_rankFusion_internal_fields>": {
                            "HASH<matchAuthor_rank>": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$HASH<_internal_rankFusion_internal_fields>.HASH<matchAuthor_rank>",
                                            "?number"
                                        ]
                                    },
                                    "?string",
                                    "$HASH<_internal_rankFusion_internal_fields>.HASH<matchAuthor_rank>"
                                ]
                            },
                            "HASH<matchDistance_rank>": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$HASH<_internal_rankFusion_internal_fields>.HASH<matchDistance_rank>",
                                            "?number"
                                        ]
                                    },
                                    "?string",
                                    "$HASH<_internal_rankFusion_internal_fields>.HASH<matchDistance_rank>"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$HASH<_internal_rankFusion_internal_fields>.HASH<matchAuthor_score>",
                                "$HASH<_internal_rankFusion_internal_fields>.HASH<matchDistance_score>"
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
                        "HASH<_internal_rankFusion_internal_fields>": false,
                        "HASH<_id>": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, RepresentativeQueryShape) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ],
                    matchDistance: [
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
            combination: {
                weights: {
                    matchAuthor: 2,
                    matchDistance: 3
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), expCtx);
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
                    "$sort": {
                        "author": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            1,
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                                    1
                                                ]
                                            }
                                        ]
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
                                "$replaceRoot": {
                                    "newRoot": {
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchDistance_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchDistance_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        1,
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchDistance_rank",
                                                                1
                                                            ]
                                                        }
                                                    ]
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
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "matchAuthor_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                        1
                                    ]
                                },
                                "matchDistance_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchDistance_score",
                                        1
                                    ]
                                }
                            }
                        }
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "$const": {
                                        "?": "?"
                                    }
                                },
                                "in": {
                                    "matchAuthor_score": {
                                        "$max": [
                                            "$$value.matchAuthor_score",
                                            "$$this.matchAuthor_score"
                                        ]
                                    },
                                    "matchDistance_score": {
                                        "$max": [
                                            "$$value.matchDistance_score",
                                            "$$this.matchDistance_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchAuthor_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                            1
                                        ]
                                    },
                                    "?",
                                    "$_internal_rankFusion_internal_fields.matchAuthor_rank"
                                ]
                            },
                            "matchDistance_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchDistance_rank",
                                            1
                                        ]
                                    },
                                    "?",
                                    "$_internal_rankFusion_internal_fields.matchDistance_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                "$_internal_rankFusion_internal_fields.matchDistance_score"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);


    // Ensure the representative query shape is reparseable.
    ASSERT_DOES_NOT_THROW(Pipeline::parseFromArray(asOneObj.firstElement(), expCtx));
}

TEST_F(DocumentSourceRankFusionTest, CheckOnePipelineRankFusionFullDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                }
            },
            combination: {
                weights: {
                    agatha: 5
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
                                            }
                                        ]
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
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "agatha_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "agatha_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "agatha_score": {
                                        "$max": [
                                            "$$value.agatha_score",
                                            "$$this.agatha_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.agatha_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, CheckTwoPipelineRankFusionFullDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ],
                    searchPipe: [
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
            combination: {
                weights: {
                    searchPipe: 2
                }
            }
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                                    {
                                                        "$const": 60
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.searchPipe_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "searchPipe_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                                                {
                                                                    "$const": 60
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
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "agatha_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.agatha_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "searchPipe_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.searchPipe_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                }
                            }
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "agatha_score": {
                                        "$const": 0
                                    },
                                    "searchPipe_score": {
                                        "$const": 0
                                    }
                                },
                                "in": {
                                    "agatha_score": {
                                        "$max": [
                                            "$$value.agatha_score",
                                            "$$this.agatha_score"
                                        ]
                                    },
                                    "searchPipe_score": {
                                        "$max": [
                                            "$$value.searchPipe_score",
                                            "$$this.searchPipe_score"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.agatha_rank"
                                ]
                            },
                            "searchPipe_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.searchPipe_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score",
                                "$_internal_rankFusion_internal_fields.searchPipe_score"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, CheckFourPipelinesScoreDetailsDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(ResolvedNamespaceMap{
        {expCtx->getNamespaceString(), {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchWithTextScore: [
                        { $match: { $text: { $search: "Agatha Christie" } } },
                        { $sort: {author: 1} }
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
                    ],
                    vectorSearchPipe: [
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
                    matchWithoutTextScore: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
                    ]
                }
            },
            combination: {
                weights: {
                    matchWithTextScore: 3,
                    searchPipe: 2,
                    vectorSearchPipe: 4,
                    matchWithoutTextScore: 5
                }
            },
            scoreDetails: true
        }
    })");

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
    const auto pipeline = Pipeline::create(desugaredList, getExpCtx());
    BSONObj asOneObj = BSON("expectedStages" << pipeline->serializeToBson());
    // The expected desugar is too large for the compiler so we need to split it up.
    const std::string expectedStages = std::string(R"({
            "expectedStages": [)") +
        std::string(R"({
                    "$match": {
                        "$text": {
                            "$search": "Agatha Christie"
                        }
                    }
                },
                {
                    "$sort": {
                        "author": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "_internal_rankFusion_docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.matchWithTextScore_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchWithTextScore_score": {
                                "$multiply": [
                                    {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$_internal_rankFusion_internal_fields.matchWithTextScore_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
                                            }
                                        ]
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
                        "_internal_rankFusion_internal_fields": {
                            "matchWithTextScore_scoreDetails": {
                                "value": {
                                    "$meta": "score"
                                },
                                "details": []
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
                                "$match": {
                                    "author": "Agatha Christie"
                                }
                            },
                            {
                                "$sort": {
                                    "author": 1,
                                    "$_internalOutputSortKeyMetadata": true
                                }
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.matchWithoutTextScore_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchWithoutTextScore_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.matchWithoutTextScore_rank",
                                                                {
                                                                    "$const": 60
                                                                }
                                                            ]
                                                        }
                                                    ]
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
                                    "_internal_rankFusion_internal_fields": {
                                        "matchWithoutTextScore_scoreDetails": {
                                            "details": []
                                        }
                                    }
                                }
                            }
                        ]
                    }
                },)") +
        std::string(R"(
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.searchPipe_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "searchPipe_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                                                {
                                                                    "$const": 60
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
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "searchPipe_scoreDetails": {
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
                                        "_internal_rankFusion_docs": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.vectorSearchPipe_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "vectorSearchPipe_score": {
                                            "$multiply": [
                                                {
                                                    "$divide": [
                                                        {
                                                            "$const": 1
                                                        },
                                                        {
                                                            "$add": [
                                                                "$_internal_rankFusion_internal_fields.vectorSearchPipe_rank",
                                                                {
                                                                    "$const": 60
                                                                }
                                                            ]
                                                        }
                                                    ]
                                                },
                                                {
                                                    "$const": 4
                                                }
                                            ]
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields": {
                                        "vectorSearchPipe_scoreDetails": {
                                            "value": {
                                                "$meta": "score"
                                            },
                                            "details": []
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
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "_internal_rankFusion_internal_fields": {
                            "$push": {
                                "matchWithTextScore_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchWithTextScore_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchWithTextScore_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchWithTextScore_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchWithTextScore_scoreDetails": "$_internal_rankFusion_internal_fields.matchWithTextScore_scoreDetails",
                                "matchWithoutTextScore_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchWithoutTextScore_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchWithoutTextScore_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.matchWithoutTextScore_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "matchWithoutTextScore_scoreDetails": "$_internal_rankFusion_internal_fields.matchWithoutTextScore_scoreDetails",
                                "searchPipe_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.searchPipe_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "searchPipe_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "searchPipe_scoreDetails": "$_internal_rankFusion_internal_fields.searchPipe_scoreDetails",
                                "vectorSearchPipe_score": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.vectorSearchPipe_score",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "vectorSearchPipe_rank": {
                                    "$ifNull": [
                                        "$_internal_rankFusion_internal_fields.vectorSearchPipe_rank",
                                        {
                                            "$const": 0
                                        }
                                    ]
                                },
                                "vectorSearchPipe_scoreDetails": "$_internal_rankFusion_internal_fields.vectorSearchPipe_scoreDetails"
                            }
                        },
                        "$willBeMerged": false
                    }
                },)") +
        std::string(R"(
                {
                    "$project": {
                        "_id": true,
                        "_internal_rankFusion_docs": true,
                        "_internal_rankFusion_internal_fields": {
                            "$reduce": {
                                "input": "$_internal_rankFusion_internal_fields",
                                "initialValue": {
                                    "matchWithTextScore_score": {
                                        "$const": 0
                                    },
                                    "matchWithTextScore_rank": {
                                        "$const": 0
                                    },
                                    "matchWithTextScore_scoreDetails": {
                                        "$const": {}
                                    },
                                    "matchWithoutTextScore_score": {
                                        "$const": 0
                                    },
                                    "matchWithoutTextScore_rank": {
                                        "$const": 0
                                    },
                                    "matchWithoutTextScore_scoreDetails": {
                                        "$const": {}
                                    },
                                    "searchPipe_score": {
                                        "$const": 0
                                    },
                                    "searchPipe_rank": {
                                        "$const": 0
                                    },
                                    "searchPipe_scoreDetails": {
                                        "$const": {}
                                    },
                                    "vectorSearchPipe_score": {
                                        "$const": 0
                                    },
                                    "vectorSearchPipe_rank": {
                                        "$const": 0
                                    },
                                    "vectorSearchPipe_scoreDetails": {
                                        "$const": {}
                                    }
                                },
                                "in": {
                                    "matchWithTextScore_score": {
                                        "$max": [
                                            "$$value.matchWithTextScore_score",
                                            "$$this.matchWithTextScore_score"
                                        ]
                                    },
                                    "matchWithTextScore_rank": {
                                        "$max": [
                                            "$$value.matchWithTextScore_rank",
                                            "$$this.matchWithTextScore_rank"
                                        ]
                                    },
                                    "matchWithTextScore_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.matchWithTextScore_scoreDetails",
                                            "$$this.matchWithTextScore_scoreDetails"
                                        ]
                                    },
                                    "matchWithoutTextScore_score": {
                                        "$max": [
                                            "$$value.matchWithoutTextScore_score",
                                            "$$this.matchWithoutTextScore_score"
                                        ]
                                    },
                                    "matchWithoutTextScore_rank": {
                                        "$max": [
                                            "$$value.matchWithoutTextScore_rank",
                                            "$$this.matchWithoutTextScore_rank"
                                        ]
                                    },
                                    "matchWithoutTextScore_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.matchWithoutTextScore_scoreDetails",
                                            "$$this.matchWithoutTextScore_scoreDetails"
                                        ]
                                    },
                                    "searchPipe_score": {
                                        "$max": [
                                            "$$value.searchPipe_score",
                                            "$$this.searchPipe_score"
                                        ]
                                    },
                                    "searchPipe_rank": {
                                        "$max": [
                                            "$$value.searchPipe_rank",
                                            "$$this.searchPipe_rank"
                                        ]
                                    },
                                    "searchPipe_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.searchPipe_scoreDetails",
                                            "$$this.searchPipe_scoreDetails"
                                        ]
                                    },
                                    "vectorSearchPipe_score": {
                                        "$max": [
                                            "$$value.vectorSearchPipe_score",
                                            "$$this.vectorSearchPipe_score"
                                        ]
                                    },
                                    "vectorSearchPipe_rank": {
                                        "$max": [
                                            "$$value.vectorSearchPipe_rank",
                                            "$$this.vectorSearchPipe_rank"
                                        ]
                                    },
                                    "vectorSearchPipe_scoreDetails": {
                                        "$mergeObjects": [
                                            "$$value.vectorSearchPipe_scoreDetails",
                                            "$$this.vectorSearchPipe_scoreDetails"
                                        ]
                                    }
                                }
                            }
                        }
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                "$$ROOT"
                            ]
                        }
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_docs": false,
                        "_id": true
                    }
                },)") +
        std::string(R"(
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "matchWithTextScore_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchWithTextScore_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchWithTextScore_rank"
                                ]
                            },
                            "matchWithoutTextScore_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.matchWithoutTextScore_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.matchWithoutTextScore_rank"
                                ]
                            },
                            "searchPipe_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.searchPipe_rank"
                                ]
                            },
                            "vectorSearchPipe_rank": {
                                "$cond": [
                                    {
                                        "$eq": [
                                            "$_internal_rankFusion_internal_fields.vectorSearchPipe_rank",
                                            {
                                                "$const": 0
                                            }
                                        ]
                                    },
                                    {
                                        "$const": "NA"
                                    },
                                    "$_internal_rankFusion_internal_fields.vectorSearchPipe_rank"
                                ]
                            }
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.matchWithTextScore_score",
                                "$_internal_rankFusion_internal_fields.matchWithoutTextScore_score",
                                "$_internal_rankFusion_internal_fields.searchPipe_score",
                                "$_internal_rankFusion_internal_fields.vectorSearchPipe_score"
                            ]
                        }
                    }
                },)") +
        std::string(R"(
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "calculatedScoreDetails": [
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "matchWithTextScore"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.matchWithTextScore_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.matchWithTextScore_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 3
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.matchWithTextScore_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "matchWithoutTextScore"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.matchWithoutTextScore_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.matchWithoutTextScore_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 5
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.matchWithoutTextScore_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "searchPipe"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 2
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.searchPipe_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": {
                                                "$const": "vectorSearchPipe"
                                            },
                                            "rank": "$_internal_rankFusion_internal_fields.vectorSearchPipe_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.vectorSearchPipe_rank",
                                                            {
                                                                "$const": "NA"
                                                            }
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    {
                                                        "$const": 4
                                                    }
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.vectorSearchPipe_scoreDetails"
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
                                "$const": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:"
                            },
                            "details": "$_internal_rankFusion_internal_fields.calculatedScoreDetails"
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
                        "_internal_rankFusion_internal_fields": false,
                        "_id": true
                    }
                }
            ]
        })");
    ASSERT_BSONOBJ_EQ_AUTO(expectedStages, asOneObj);
}
}  // namespace
}  // namespace mongo
