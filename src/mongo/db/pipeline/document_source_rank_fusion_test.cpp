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

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
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
            input: {pipelines: "not an array"}
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
                pipelines: []
            }
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
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
                        "author": 1
                    }
                },
                {
                    "$group": {
                        "_id": {
                            "$const": null
                        },
                        "docs": {
                            "$push": "$$ROOT"
                        }
                    }
                },
                {
                    "$unwind": {
                        "path": "$docs",
                        "includeArrayIndex": "agatha_rank"
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
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

TEST_F(DocumentSourceRankFusionTest, CheckMultiplePipelinesAndOptionalArgumentsAllowed) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        StringMap<ResolvedNamespace>{{expCtx->getNamespaceString().coll().toString(),
                                      {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});

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
                        "author": 1
                    }
                },
                {
                    "$group": {
                        "_id": {
                            "$const": null
                        },
                        "docs": {
                            "$push": "$$ROOT"
                        }
                    }
                },
                {
                    "$unwind": {
                        "path": "$docs",
                        "includeArrayIndex": "matchAuthor_rank"
                    }
                },
                {
                    "$addFields": {
                        "matchAuthor_score": {
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
                                "$group": {
                                    "_id": {
                                        "$const": null
                                    },
                                    "docs": {
                                        "$push": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$unwind": {
                                    "path": "$docs",
                                    "includeArrayIndex": "matchGenres_rank"
                                }
                            },
                            {
                                "$addFields": {
                                    "matchGenres_score": {
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
                                "$group": {
                                    "_id": {
                                        "$const": null
                                    },
                                    "docs": {
                                        "$push": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$unwind": {
                                    "path": "$docs",
                                    "includeArrayIndex": "matchPlot_rank"
                                }
                            },
                            {
                                "$addFields": {
                                    "matchPlot_score": {
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
                       9191102);
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
                       16436);  // Unrecognized stage.
}

TEST_F(DocumentSourceRankFusionTest, CheckLimitSampleUnionwithAllowed) {
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

    const auto desugaredList =
        DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx());
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
                    "$sort": {
                        "author": 1
                    }
                },
                {
                    "$limit": 10
                },
                {
                    "$group": {
                        "_id": {
                            "$const": null
                        },
                        "docs": {
                            "$push": "$$ROOT"
                        }
                    }
                },
                {
                    "$unwind": {
                        "path": "$docs",
                        "includeArrayIndex": "sample_rank"
                    }
                },
                {
                    "$addFields": {
                        "sample_score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        "$sample_rank",
                                        {
                                            "$const": 60
                                        }
                                    ]
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
                                "$sort": {
                                    "author": 1
                                }
                            },
                            {
                                "$group": {
                                    "_id": {
                                        "$const": null
                                    },
                                    "docs": {
                                        "$push": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$unwind": {
                                    "path": "$docs",
                                    "includeArrayIndex": "unionWith_rank"
                                }
                            },
                            {
                                "$addFields": {
                                    "unionWith_score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$unionWith_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
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
                        "sample_score": {
                            "$max": {
                                "$ifNull": [
                                    "$sample_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "unionWith_score": {
                            "$max": {
                                "$ifNull": [
                                    "$unionWith_score",
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
                                "$sample_score",
                                "$unionWith_score"
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

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNestedUnionWithModifiesFields) {
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

TEST_F(DocumentSourceRankFusionTest, CheckGeoNearAllowedWhenNoIncludeLocs) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(
        StringMap<ResolvedNamespace>{{expCtx->getNamespaceString().coll().toString(),
                                      {expCtx->getNamespaceString(), std::vector<BSONObj>()}}});
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
                        "author": 1
                    }
                },
                {
                    "$group": {
                        "_id": {
                            "$const": null
                        },
                        "docs": {
                            "$push": "$$ROOT"
                        }
                    }
                },
                {
                    "$unwind": {
                        "path": "$docs",
                        "includeArrayIndex": "agatha_rank"
                    }
                },
                {
                    "$addFields": {
                        "agatha_score": {
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
                                    "distanceField": "dist.calculated",
                                    "maxDistance": 2,
                                    "query": {
                                        "category": {$eq: "Parks"}
                                    },
                                    "spherical": true
                                }
                            },
                            {
                                "$group": {
                                    "_id": {
                                        "$const": null
                                    },
                                    "docs": {
                                        "$push": "$$ROOT"
                                    }
                                }
                            },
                            {
                                "$unwind": {
                                    "path": "$docs",
                                    "includeArrayIndex": "geo_rank"
                                }
                            },
                            {
                                "$addFields": {
                                    "geo_score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$geo_rank",
                                                    {
                                                        "$const": 60
                                                    }
                                                ]
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
                        "geo_score": {
                            "$max": {
                                "$ifNull": [
                                    "$geo_score",
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
                                "$geo_score"
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
                                distanceField: "dist.calculated",
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
                       9191101);
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

}  // namespace
}  // namespace mongo
