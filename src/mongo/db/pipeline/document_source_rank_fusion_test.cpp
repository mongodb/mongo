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

#include "expression_context.h"
#include "mongo/bson/json.h"

#include "mongo/unittest/assert.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/util/assert_util.h"

#include <regex>

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
class DocumentSourceRankFusionTest : public AggregationContextFixture {
private:
    RAIIServerParameterControllerForTest featureFlagController{"featureFlagRankFusionBasic", true};
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

TEST_F(DocumentSourceRankFusionTest, CheckOnePipelineAllowed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match : { author : "Agatha Christie" } },
                        { $skip: 5 },
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
                    "$skip": 5
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$agatha_rank",
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
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_score",
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
                                "$agatha_score"
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

TEST_F(DocumentSourceRankFusionTest, CheckMultiplePipelinesAndOptionalArgumentsAllowed) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});

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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$matchAuthor_rank",
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
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$matchGenres_rank",
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "matchPlot_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchPlot_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$matchPlot_rank",
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
                        "matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchPlot_score",
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
                                "$matchAuthor_score",
                                "$matchGenres_score",
                                "$matchPlot_score"
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
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
    auto nsToUnionWith1 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "shortstories");
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
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
    auto nsToUnionWith1 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "shortstories");
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

TEST_F(DocumentSourceRankFusionTest, CheckGeoNearNotAllowed) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
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
                       9191100);
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
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});

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
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});

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
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$matchAuthor_rank",
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
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$matchGenres_rank",
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$matchAuthor_score",
                                "$matchGenres_score"
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

// Same as CheckWeightsApplied but the ordering of fields doesn't match between input.pipelines and
// combination.weights; checks that the weights are applied to the pipeline with the same name.
TEST_F(DocumentSourceRankFusionTest, CheckWeightsAppliedToCorrectPipeline) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$matchAuthor_rank",
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
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$matchGenres_rank",
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$matchAuthor_score",
                                "$matchGenres_score"
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

TEST_F(DocumentSourceRankFusionTest, CheckWeightsAppliedMultiplePipelines) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
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
            },
            combination: {
                weights: {
                    matchGenres: 3,
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$matchAuthor_rank",
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
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "matchGenres_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$matchGenres_rank",
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "matchPlot_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchPlot_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$matchPlot_rank",
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
                        "matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchPlot_score",
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
                                "$matchAuthor_score",
                                "$matchGenres_score",
                                "$matchPlot_score"
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$agatha_rank",
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
                },
                {
                    "$addFields": {
                        "agatha_scoreDetails": {
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
                        "agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "agatha_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "agatha_scoreDetails": {
                            "$mergeObjects": "$agatha_scoreDetails"
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$agatha_score"
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
                                            "$const": "agatha"
                                        },
                                        "rank": "$agatha_rank",
                                        "weight": {
                                            "$const": 5
                                        }
                                    },
                                    "$agatha_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": "$score",
                            "description": {
                                "$const": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:"
                            },
                            "details": "$calculatedScoreDetails"
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
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$agatha_rank",
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
                },
                {
                    "$addFields": {
                        "agatha_scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
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
                        "agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "agatha_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "agatha_scoreDetails": {
                            "$mergeObjects": "$agatha_scoreDetails"
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$agatha_score"
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
                                            "$const": "agatha"
                                        },
                                        "rank": "$agatha_rank",
                                        "weight": {
                                            "$const": 5
                                        }
                                    },
                                    "$agatha_scoreDetails"
                                ]
                            }
                        ]
                    }
                },
                {
                    "$setMetadata": {
                        "scoreDetails": {
                            "value": "$score",
                            "description": {
                                "$const": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:"
                            },
                            "details": "$calculatedScoreDetails"
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

TEST_F(DocumentSourceRankFusionTest, CheckTwoPipelineScoreDetailsDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$agatha_rank",
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
                },
                {
                    "$addFields": {
                        "agatha_scoreDetails": {
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
                                    "index": "search_index",
                                    "text": {
                                        "query": "mystery",
                                        "path": "genres"
                                    },
                                    "scoreDetails": true
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "searchPipe_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$searchPipe_rank",
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
                            },
                            {
                                "$addFields": {
                                    "searchPipe_scoreDetails": {
                                        "$meta": "scoreDetails"
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
                        "agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "agatha_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "agatha_scoreDetails": {
                            "$mergeObjects": "$agatha_scoreDetails"
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
                        "searchPipe_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$searchPipe_rank",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "searchPipe_scoreDetails": {
                            "$mergeObjects": "$searchPipe_scoreDetails"
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$agatha_score",
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
                                            "$const": "agatha"
                                        },
                                        "rank": "$agatha_rank",
                                        "weight": {
                                            "$const": 1
                                        }
                                    },
                                    "$agatha_scoreDetails"
                                ]
                            },
                            {
                                "$mergeObjects": [
                                    {
                                        "inputPipelineName": {
                                            "$const": "searchPipe"
                                        },
                                        "rank": "$searchPipe_rank",
                                        "weight": {
                                            "$const": 2
                                        }
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
                            "value": "$score",
                            "description": {
                                "$const": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:"
                            },
                            "details": "$calculatedScoreDetails"
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
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: {author: 1} }
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
            },
            combination: {
                weights: {
                    matchAuthor: 2,
                    matchPlot: 3
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
                            "HASH<docs>": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "HASH<order>": 1
                        },
                        "output": {
                            "HASH<matchAuthor_rank>": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "HASH<matchAuthor_score>": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        "?number",
                                        {
                                            "$add": [
                                                "$HASH<matchAuthor_rank>",
                                                "?number"
                                            ]
                                        }
                                    ]
                                },
                                "?number"
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "HASH<pipeline_test>",
                        "pipeline": [
                            {
                                "$vectorSearch": "?object"
                            },
                            {
                                "$replaceRoot": {
                                    "newRoot": {
                                        "HASH<docs>": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "HASH<order>": 1
                                    },
                                    "output": {
                                        "HASH<matchPlot_rank>": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "HASH<matchPlot_score>": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    "?number",
                                                    {
                                                        "$add": [
                                                            "$HASH<matchPlot_rank>",
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
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$HASH<docs>.HASH<_id>",
                        "HASH<docs>": {
                            "$first": "$HASH<docs>"
                        },
                        "HASH<matchAuthor_score>": {
                            "$max": {
                                "$ifNull": [
                                    "$HASH<matchAuthor_score>",
                                    "?number"
                                ]
                            }
                        },
                        "HASH<matchPlot_score>": {
                            "$max": {
                                "$ifNull": [
                                    "$HASH<matchPlot_score>",
                                    "?number"
                                ]
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "HASH<score>": {
                            "$add": [
                                "$HASH<matchAuthor_score>",
                                "$HASH<matchPlot_score>"
                            ]
                        }
                    }
                },
                {
                    "$sort": {
                        "HASH<score>": -1,
                        "HASH<_id>": 1
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": "$HASH<docs>"
                    }
                }
            ]
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, RepresentativeQueryShape) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match : { author : "Agatha Christie" } },
                        { $sort: { author: 1 } }
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
            },
            combination: {
                weights: {
                    matchAuthor: 2,
                    matchPlot: 3
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "matchAuthor_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$matchAuthor_rank",
                                                1
                                            ]
                                        }
                                    ]
                                },
                                1
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
                                    "?": "?"
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "matchPlot_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "matchPlot_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    1,
                                                    {
                                                        "$add": [
                                                            "$matchPlot_rank",
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
                                    1
                                ]
                            }
                        },
                        "matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$matchPlot_score",
                                    1
                                ]
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$matchAuthor_score",
                                "$matchPlot_score"
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$agatha_rank",
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
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_score",
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
                                "$agatha_score"
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$agatha_score"
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

TEST_F(DocumentSourceRankFusionTest, CheckTwoPipelineRankFusionFullDesugaring) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRankFusionFull", true);
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        ResolvedNamespaceMap{{expCtx->ns, {expCtx->ns, std::vector<BSONObj>()}}});
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
                            "docs": "$$ROOT"
                        }
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "agatha_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        {
                                            "$const": 1
                                        },
                                        {
                                            "$add": [
                                                "$agatha_rank",
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
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
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
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "searchPipe_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "searchPipe_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    {
                                                        "$const": 1
                                                    },
                                                    {
                                                        "$add": [
                                                            "$searchPipe_rank",
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
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$docs._id",
                        "docs": {
                            "$first": "$docs"
                        },
                        "agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$agatha_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
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
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$agatha_score",
                                "$searchPipe_score"
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$agatha_score",
                                "$searchPipe_score"
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
