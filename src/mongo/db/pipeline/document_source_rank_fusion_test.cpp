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
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "agatha_score": "$__hs_agatha_score"
                                    }
                                }
                            ]
                        }
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
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchGenres_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchPlot_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchGenres_score": "$__hs_matchGenres_score",
                                        "matchPlot_score": "$__hs_matchPlot_score"
                                    }
                                }
                            ]
                        }
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
                },
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
        })",
        asOneObj);
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
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_geo_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.geo_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "agatha_score": "$__hs_agatha_score",
                                        "geo_score": "$__hs_geo_score"
                                    }
                                }
                            ]
                        }
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
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchGenres_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchGenres_score": "$__hs_matchGenres_score"
                                    }
                                }
                            ]
                        }
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
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchGenres_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchGenres_score": "$__hs_matchGenres_score"
                                    }
                                }
                            ]
                        }
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
    auto expected = std::string(R"({
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
                })") +
        R"(,
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchDistance_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchDistance_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchGenres_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchPlot_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
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
        })";  // NOLINT
    ASSERT_BSONOBJ_EQ(fromjson(expected), asOneObj);
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
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_agatha_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_agatha_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "agatha_score": "$__hs_agatha_score",
                                        "agatha_rank": "$__hs_agatha_rank",
                                        "agatha_scoreDetails": "$__hs_agatha_scoreDetails"
                                    }
                                }
                            ]
                        }
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
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_agatha_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_agatha_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "agatha_score": "$__hs_agatha_score",
                                        "agatha_rank": "$__hs_agatha_rank",
                                        "agatha_scoreDetails": "$__hs_agatha_scoreDetails"
                                    }
                                }
                            ]
                        }
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
    auto expected = std::string(R"({
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
                    "$addFields": {
                        "_internal_rankFusion_internal_fields": {
                            "agatha_scoreDetails": {
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
                },
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_agatha_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_agatha_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
                        },
                        "__hs_searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.searchPipe_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.searchPipe_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                })") +
        R"(,
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "agatha_score": "$__hs_agatha_score",
                                        "agatha_rank": "$__hs_agatha_rank",
                                        "agatha_scoreDetails": "$__hs_agatha_scoreDetails",
                                        "searchPipe_score": "$__hs_searchPipe_score",
                                        "searchPipe_rank": "$__hs_searchPipe_rank",
                                        "searchPipe_scoreDetails": "$__hs_searchPipe_scoreDetails"
                                    }
                                }
                            ]
                        }
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
        })";  // NOLINT
    ASSERT_BSONOBJ_EQ(fromjson(expected), asOneObj);
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
                        "HASH<__hs_matchAuthor_score>": {
                            "$max": {
                                "$ifNull": [
                                    "$HASH<_internal_rankFusion_internal_fields>.HASH<matchAuthor_score>",
                                    "?number"
                                ]
                            }
                        },
                        "HASH<__hs_matchDistance_score>": {
                            "$max": {
                                "$ifNull": [
                                    "$HASH<_internal_rankFusion_internal_fields>.HASH<matchDistance_score>",
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
                                "$HASH<_internal_rankFusion_docs>",
                                {
                                    "HASH<_internal_rankFusion_internal_fields>": {
                                        "HASH<matchAuthor_score>": "$HASH<__hs_matchAuthor_score>",
                                        "HASH<matchDistance_score>": "$HASH<__hs_matchDistance_score>"
                                    }
                                }
                            ]
                        }
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
                        "__hs_matchAuthor_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchAuthor_score",
                                    1
                                ]
                            }
                        },
                        "__hs_matchDistance_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchDistance_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchAuthor_score": "$__hs_matchAuthor_score",
                                        "matchDistance_score": "$__hs_matchDistance_score"
                                    }
                                }
                            ]
                        }
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
    ASSERT_DOES_NOT_THROW(pipeline_factory::makePipeline(
        asOneObj.firstElement(), expCtx, {.attachCursorSource = false}));
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
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "agatha_score": "$__hs_agatha_score"
                                    }
                                }
                            ]
                        }
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
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.searchPipe_score",
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
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "agatha_score": "$__hs_agatha_score",
                                        "searchPipe_score": "$__hs_searchPipe_score"
                                    }
                                }
                            ]
                        }
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
    auto expected = std::string(R"({
            "expectedStages": [
                {
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
                },
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
                })") +
        R"(,
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_matchWithTextScore_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchWithTextScore_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchWithTextScore_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchWithTextScore_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchWithTextScore_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.matchWithTextScore_scoreDetails"
                        },
                        "__hs_matchWithoutTextScore_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchWithoutTextScore_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchWithoutTextScore_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchWithoutTextScore_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_matchWithoutTextScore_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.matchWithoutTextScore_scoreDetails"
                        },
                        "__hs_searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.searchPipe_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_searchPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.searchPipe_scoreDetails"
                        },
                        "__hs_vectorSearchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.vectorSearchPipe_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_vectorSearchPipe_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.vectorSearchPipe_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "__hs_vectorSearchPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.vectorSearchPipe_scoreDetails"
                        },
                        "$willBeMerged": false
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "$mergeObjects": [
                                "$_internal_rankFusion_docs",
                                {
                                    "_internal_rankFusion_internal_fields": {
                                        "matchWithTextScore_score": "$__hs_matchWithTextScore_score",
                                        "matchWithTextScore_rank": "$__hs_matchWithTextScore_rank",
                                        "matchWithTextScore_scoreDetails": "$__hs_matchWithTextScore_scoreDetails",
                                        "matchWithoutTextScore_score": "$__hs_matchWithoutTextScore_score",
                                        "matchWithoutTextScore_rank": "$__hs_matchWithoutTextScore_rank",
                                        "matchWithoutTextScore_scoreDetails": "$__hs_matchWithoutTextScore_scoreDetails",
                                        "searchPipe_score": "$__hs_searchPipe_score",
                                        "searchPipe_rank": "$__hs_searchPipe_rank",
                                        "searchPipe_scoreDetails": "$__hs_searchPipe_scoreDetails",
                                        "vectorSearchPipe_score": "$__hs_vectorSearchPipe_score",
                                        "vectorSearchPipe_rank": "$__hs_vectorSearchPipe_rank",
                                        "vectorSearchPipe_scoreDetails": "$__hs_vectorSearchPipe_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
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
                })" +
        R"(,
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
                },
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
        })";  // NOLINT
    ASSERT_BSONOBJ_EQ(fromjson(expected), asOneObj);
}
TEST_F(DocumentSourceRankFusionTest, InternalFieldBehaviorThroughGroupAndReshape) {
    auto spec = fromjson(R"({
         $rankFusion: {
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

    // Build the desugared pipeline to extract the $group and subsequent stages.
    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());

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
    // in _internal_rankFusion_docs and includes:
    //   - a field with the __hs_ prefix that will survive the pipeline,
    //   - a field named "_internal_rankFusion_internal_fields" that collides with the internal
    //     name and will be lost.
    auto mock = exec::agg::MockStage::createForTest(
        {
            Document{{"_internal_rankFusion_docs",
                      Document{{"_id", 1},
                               {"val", 10},
                               {"__hs_custom", "alpha"_sd},
                               {"_internal_rankFusion_internal_fields", "user_data"_sd}}},
                     {"_internal_rankFusion_internal_fields", Document{{"name1_score", 3.0}}}},
            Document{{"_internal_rankFusion_docs",
                      Document{{"_id", 2}, {"val", 20}, {"__hs_custom", "beta"_sd}}},
                     {"_internal_rankFusion_internal_fields", Document{{"name1_score", 7.0}}}},
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

    // A user field named "_internal_rankFusion_internal_fields" is lost: $mergeObjects overwrites
    // it with the repacked internal fields object, and then $project removes it.
    ASSERT_TRUE(results[0]["_internal_rankFusion_internal_fields"_sd].missing());
    ASSERT_TRUE(results[1]["_internal_rankFusion_internal_fields"_sd].missing());

    // All internal fields are removed from the output.
    ASSERT_TRUE(results[0]["_internal_rankFusion_docs"_sd].missing());
    ASSERT_TRUE(results[1]["_internal_rankFusion_docs"_sd].missing());
    ASSERT_TRUE(results[0]["__hs_name1_score"_sd].missing());
    ASSERT_TRUE(results[1]["__hs_name1_score"_sd].missing());
}
// Tests for DocumentSourceRankFusion::LiteParsed::validate().
// These call validate() directly on the LiteParsed object.

TEST_F(DocumentSourceRankFusionTest, ValidateSucceedsWithValidRankedPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = DocumentSourceRankFusion::LiteParsed::parse(nss, spec.firstElement(), {});
    liteParsed->validate();  // Should not throw.
}

TEST_F(DocumentSourceRankFusionTest, ValidateThrowsOnEmptySubpipeline) {
    auto nss = getExpCtx()->getNamespaceString();
    std::vector<LiteParsedPipeline> pipelines;
    pipelines.emplace_back(nss, std::vector<BSONObj>{});

    auto spec =
        BSON("$rankFusion" << BSON("input" << BSON("pipelines" << BSON("p1" << BSONArray()))));
    auto liteParsed = std::make_unique<DocumentSourceRankFusion::LiteParsed>(
        spec.firstElement(), nss, std::move(pipelines));
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108700);
}

TEST_F(DocumentSourceRankFusionTest, ValidateThrowsOnNonRankedPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$match: {x: 1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = DocumentSourceRankFusion::LiteParsed::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108702);
}

TEST_F(DocumentSourceRankFusionTest, ValidateThrowsOnNonSelectionStage) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}, {$addFields: {y: 1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = DocumentSourceRankFusion::LiteParsed::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108704);
}

TEST_F(DocumentSourceRankFusionTest, ValidateThrowsOnNestedHybridSearch) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$rankFusion: {input: {pipelines: {inner: [{$sort: {x: 1}}]}}}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = DocumentSourceRankFusion::LiteParsed::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108701);
}

TEST_F(DocumentSourceRankFusionTest, ValidateThrowsOnDuplicatePipelineNames) {
    // Construct BSON with duplicate pipeline names manually since fromjson deduplicates.
    auto spec =
        BSON("$rankFusion" << BSON(
                 "input" << BSON("pipelines" << BSON(
                                     "dup" << BSON_ARRAY(BSON("$sort" << BSON("x" << 1))) << "dup"
                                           << BSON_ARRAY(BSON("$sort" << BSON("y" << 1)))))));

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = DocumentSourceRankFusion::LiteParsed::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108714);
}

TEST_F(DocumentSourceRankFusionTest, ValidateThrowsOnScoreStageInPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p1: [{$sort: {x: 1}}, {$score: {}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = DocumentSourceRankFusion::LiteParsed::parse(nss, spec.firstElement(), {});
    ASSERT_THROWS_CODE(liteParsed->validate(), AssertionException, 12108703);
}

TEST_F(DocumentSourceRankFusionTest, ValidateSucceedsWithMultipleValidPipelines) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    byX: [{$sort: {x: 1}}],
                    byY: [{$sort: {y: -1}}]
                }
            }
        }
    })");

    auto nss = getExpCtx()->getNamespaceString();
    auto liteParsed = DocumentSourceRankFusion::LiteParsed::parse(nss, spec.firstElement(), {});
    liteParsed->validate();  // Should not throw.
}

}  // namespace
}  // namespace mongo
