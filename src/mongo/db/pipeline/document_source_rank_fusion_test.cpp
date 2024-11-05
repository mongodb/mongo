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
                {}
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::IDLFailedToParse);
}

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
                        "includeArrayIndex": "first_rank"
                    }
                },
                {
                    "$addFields": {
                        "first_score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        "$first_rank",
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
        })",
        asOneObj);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfUnknownFieldInsideInputs) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
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


TEST_F(DocumentSourceRankFusionTest, ErrorsIfAsIsNotString) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ],
                as: 1
               },
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ]
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfNotRankedPipeline) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ]
               },
               {
                pipeline: [
                    { $match : { age : 50 } }
                ]
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191100);
}

TEST_F(DocumentSourceRankFusionTest, CheckMultiplePipelinesAndOptionalArgumentsAllowed) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(StringMap<ResolvedNamespace>{
        {expCtx->ns.coll().toString(), {expCtx->ns, std::vector<BSONObj>()}}});

    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ],
                as: "matchAuthor"
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
                ],
                as: "matchGenres"
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
                ],
                as: "matchPlot"
               }
            ]
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
                        "includeArrayIndex": "first_rank"
                    }
                },
                {
                    "$addFields": {
                        "first_score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        "$first_rank",
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
                                    "includeArrayIndex": "second_rank"
                                }
                            },
                            {
                                "$addFields": {
                                    "second_score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$second_rank",
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
                        "first_score": {
                            "$max": {
                                "$ifNull": [
                                    "$first_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "second_score": {
                            "$max": {
                                "$ifNull": [
                                    "$second_score",
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
                                "$first_score",
                                "$second_score"
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
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ],
                as: "matchAuthor"
               },
               {
                pipeline: [
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
                ],
                as: "matchGenres"
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfSearchStoredSourceUsed) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ],
                as: "matchAuthor"
               },
               {
                pipeline: [
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
                ],
                as: "matchGenres"
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191102);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfInternalSearchMongotRemoteUsed) {
    RAIIServerParameterControllerForTest controller("featureFlagSearchHybridScoring", true);
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ],
                as: "matchAuthor"
               },
               {
                pipeline: [
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
                ],
                as: "matchGenres"
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       16436);
}

TEST_F(DocumentSourceRankFusionTest, CheckLimitSampleUnionwithAllowed) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(StringMap<ResolvedNamespace>{
        {expCtx->ns.coll().toString(), {expCtx->ns, std::vector<BSONObj>()}}});
    auto nsToUnionWith1 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $sample: { size: 10 } },
                    { $sort: {author: 1} },
                    { $limit: 10 }
                ]
               },
               {
                pipeline: [
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

            ]
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
                        "includeArrayIndex": "first_rank"
                    }
                },
                {
                    "$addFields": {
                        "first_score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        "$first_rank",
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
                                    "includeArrayIndex": "second_rank"
                                }
                            },
                            {
                                "$addFields": {
                                    "second_score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$second_rank",
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
                        "first_score": {
                            "$max": {
                                "$ifNull": [
                                    "$first_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "second_score": {
                            "$max": {
                                "$ifNull": [
                                    "$second_score",
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
                                "$first_score",
                                "$second_score"
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
    auto nsToUnionWith1 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "novels");
    expCtx->addResolvedNamespaces({nsToUnionWith1});
    auto nsToUnionWith2 =
        NamespaceString::createNamespaceString_forTest(expCtx->ns.dbName(), "shortstories");
    expCtx->addResolvedNamespaces({nsToUnionWith2});

    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $sample: { size: 10 } },
                    { $sort: {author: 1} },
                    { $limit: 10 }
                ]
               },
               {
                pipeline: [
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
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), expCtx),
                       AssertionException,
                       9191103);
}

TEST_F(DocumentSourceRankFusionTest, CheckGeoNearAllowedWhenNoIncludeLocs) {
    auto expCtx = getExpCtx();
    expCtx->setResolvedNamespaces(StringMap<ResolvedNamespace>{
        {expCtx->ns.coll().toString(), {expCtx->ns, std::vector<BSONObj>()}}});
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ]
               },
               {
                pipeline: [
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
            ]
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
                        "includeArrayIndex": "first_rank"
                    }
                },
                {
                    "$addFields": {
                        "first_score": {
                            "$divide": [
                                {
                                    "$const": 1
                                },
                                {
                                    "$add": [
                                        "$first_rank",
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
                                        "category": {
                                            "$eq": "Parks"
                                        }
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
                                    "includeArrayIndex": "second_rank"
                                }
                            },
                            {
                                "$addFields": {
                                    "second_score": {
                                        "$divide": [
                                            {
                                                "$const": 1
                                            },
                                            {
                                                "$add": [
                                                    "$second_rank",
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
                        "first_score": {
                            "$max": {
                                "$ifNull": [
                                    "$first_score",
                                    {
                                        "$const": 0
                                    }
                                ]
                            }
                        },
                        "second_score": {
                            "$max": {
                                "$ifNull": [
                                    "$second_score",
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
                                "$first_score",
                                "$second_score"
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
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ]
               },
               {
                pipeline: [
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
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191101);
}

TEST_F(DocumentSourceRankFusionTest, ErrorsIfIncludeProject) {
    auto spec = fromjson(R"({
        $rankFusion: {
            inputs: [
               {
                pipeline: [
                    { $match : { author : "Agatha Christie" } },
                    { $sort: {author: 1} }
                ]
               },
               {
                pipeline: [
                    { $match : { age : 50 } },
                    { $sort: {author: 1} },
                    { $project: { author: 1 } }
                ]
               }
            ]
        }
    })");

    ASSERT_THROWS_CODE(DocumentSourceRankFusion::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       9191103);
}

}  // namespace
}  // namespace mongo
