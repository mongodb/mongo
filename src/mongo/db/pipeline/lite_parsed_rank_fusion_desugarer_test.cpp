/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_rank_fusion.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {
namespace {

// Convert a desugared StageSpecs vector into a single BSONObj of the form
// {expectedStages: [<stage>, <stage>, ...]} so it can be compared via ASSERT_BSONOBJ_EQ_AUTO.
BSONObj toExpectedStagesBson(const std::vector<std::unique_ptr<LiteParsedDocumentSource>>& stages) {
    BSONObjBuilder out;
    BSONArrayBuilder arr(out.subarrayStart("expectedStages"));
    for (const auto& stage : stages) {
        arr.append(stage->getOriginalBson().wrap());
    }
    arr.done();
    return out.obj();
}

// Parse the top-level pipeline (a vector of one stage) into a LiteParsedRankFusion to
// drive the desugarer.
std::unique_ptr<LiteParsedRankFusion> parseRankFusion(const NamespaceString& nss,
                                                      const BSONObj& specObj) {
    BSONObj ownedSpec = specObj.getOwned();
    auto lpds = LiteParsedDocumentSource::parse(nss, ownedSpec);
    lpds->makeOwned();
    auto* rfPtr = dynamic_cast<LiteParsedRankFusion*>(lpds.get());
    invariant(rfPtr);
    // Transfer ownership to a unique_ptr<LiteParsedRankFusion>.
    lpds.release();
    return std::unique_ptr<LiteParsedRankFusion>(rfPtr);
}

class LiteParsedRankFusionDesugarerTest : public service_context_test::WithSetupTransportLayer,
                                          public AggregationContextFixture {
protected:
    BSONObj desugar(const BSONObj& rankFusionSpecObj) {
        const NamespaceString& nss = getExpCtx()->getNamespaceString();
        auto lprf = parseRankFusion(nss, rankFusionSpecObj);
        auto desugared =
            lite_parsed_hybrid_search_desugarer::desugarRankFusion(*lprf, nss, "pipeline_test"_sd);
        return toExpectedStagesBson(desugared);
    }

private:
    unittest::ServerParameterGuard featureFlagController1{"featureFlagRankFusionBasic", true};
    unittest::ServerParameterGuard featureFlagController2{"featureFlagRankFusionFull", true};
};

TEST_F(LiteParsedRankFusionDesugarerTest, SinglePipelineBasic) {
    unittest::ServerParameterGuard disableFull("featureFlagRankFusionFull", false);
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ]
                }
            }
        }
    })");
    BSONObj actual = desugar(spec);
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
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
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
                        "_internal_rankFusion_internal_fields.agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.agatha_rank",
                                                60
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
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
                                    0
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
                                        "agatha_score": "$__hs_agatha_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.agatha_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.agatha_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.agatha_rank"
                            ]
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
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedRankFusionDesugarerTest, SinglePipelineFull) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ]
                }
            }
        }
    })");
    BSONObj actual = desugar(spec);
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
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
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
                        "_internal_rankFusion_internal_fields.agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.agatha_rank",
                                                60
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
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_agatha_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_score",
                                    0
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
                                        "agatha_score": "$__hs_agatha_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.agatha_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.agatha_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.agatha_rank"
                            ]
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedRankFusionDesugarerTest, MultiplePipelinesMixedBasic) {
    unittest::ServerParameterGuard disableFull("featureFlagRankFusionFull", false);
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    matchAuthor: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ],
                    matchGenres: [
                        { $search: { index: "search_index",
                                     text: { query: "mystery", path: "genres" } } }
                    ],
                    matchPlot: [
                        { $vectorSearch: { queryVector: [1.0, 2.0, 3.0],
                                           path: "plot_embedding",
                                           numCandidates: 300,
                                           index: "vector_index",
                                           limit: 10 } }
                    ]
                }
            }
        }
    })");
    BSONObj actual = desugar(spec);
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
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
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
                        "_internal_rankFusion_internal_fields.matchAuthor_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                                60
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
                                "$search": {
                                    "index": "search_index",
                                    "text": {
                                        "query": "mystery",
                                        "path": "genres"
                                    }
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_rankFusion_docs": "$$ROOT"
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
                                    "_internal_rankFusion_internal_fields.matchGenres_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    1,
                                                    {
                                                        "$add": [
                                                            "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                                            60
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
                                "$_internalHybridSearch": {}
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
                                "$replaceWith": {
                                    "_internal_rankFusion_docs": "$$ROOT"
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
                                    "_internal_rankFusion_internal_fields.matchPlot_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    1,
                                                    {
                                                        "$add": [
                                                            "$_internal_rankFusion_internal_fields.matchPlot_rank",
                                                            60
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
                                "$_internalHybridSearch": {}
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
                                    0
                                ]
                            }
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchGenres_score",
                                    0
                                ]
                            }
                        },
                        "__hs_matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.matchPlot_score",
                                    0
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
                        "_internal_rankFusion_internal_fields.matchAuthor_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.matchAuthor_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.matchAuthor_rank"
                            ]
                        },
                        "_internal_rankFusion_internal_fields.matchGenres_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.matchGenres_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.matchGenres_rank"
                            ]
                        },
                        "_internal_rankFusion_internal_fields.matchPlot_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.matchPlot_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.matchPlot_rank"
                            ]
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
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedRankFusionDesugarerTest, CustomWeightsBasic) {
    unittest::ServerParameterGuard disableFull("featureFlagRankFusionFull", false);
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ],
                    other: [
                        { $match: { x: 1 } },
                        { $sort: { x: 1 } }
                    ]
                }
            },
            combination: {
                weights: { agatha: 5, other: 2 }
            }
        }
    })");
    BSONObj actual = desugar(spec);
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
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
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
                        "_internal_rankFusion_internal_fields.agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.agatha_rank",
                                                60
                                            ]
                                        }
                                    ]
                                },
                                5
                            ]
                        }
                    }
                },
                {
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$match": {
                                    "x": 1
                                }
                            },
                            {
                                "$sort": {
                                    "x": 1,
                                    "$_internalOutputSortKeyMetadata": true
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_rankFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$_internalSetWindowFields": {
                                    "sortBy": {
                                        "order": 1
                                    },
                                    "output": {
                                        "_internal_rankFusion_internal_fields.other_rank": {
                                            "$rank": {}
                                        }
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_rankFusion_internal_fields.other_score": {
                                        "$multiply": [
                                            {
                                                "$divide": [
                                                    1,
                                                    {
                                                        "$add": [
                                                            "$_internal_rankFusion_internal_fields.other_rank",
                                                            60
                                                        ]
                                                    }
                                                ]
                                            },
                                            2
                                        ]
                                    }
                                }
                            },
                            {
                                "$_internalHybridSearch": {}
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
                                    0
                                ]
                            }
                        },
                        "__hs_other_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.other_score",
                                    0
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
                                        "agatha_score": "$__hs_agatha_score",
                                        "other_score": "$__hs_other_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.agatha_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.agatha_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.agatha_rank"
                            ]
                        },
                        "_internal_rankFusion_internal_fields.other_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.other_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.other_rank"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.agatha_score",
                                "$_internal_rankFusion_internal_fields.other_score"
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
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedRankFusionDesugarerTest, ScoreDetailsSortOnlyInput) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    agatha: [
                        { $match: { author: "Agatha Christie" } },
                        { $sort: { author: 1 } }
                    ]
                }
            },
            combination: { weights: { agatha: 5 } },
            scoreDetails: true
        }
    })");
    BSONObj actual = desugar(spec);
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
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
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
                        "_internal_rankFusion_internal_fields.agatha_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.agatha_rank",
                                                60
                                            ]
                                        }
                                    ]
                                },
                                5
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.agatha_scoreDetails": {
                            "details": []
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
                                    0
                                ]
                            }
                        },
                        "__hs_agatha_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.agatha_rank",
                                    0
                                ]
                            }
                        },
                        "__hs_agatha_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.agatha_scoreDetails"
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
                        "_internal_rankFusion_internal_fields.agatha_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.agatha_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.agatha_rank"
                            ]
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
                                            "inputPipelineName": "agatha",
                                            "rank": "$_internal_rankFusion_internal_fields.agatha_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.agatha_rank",
                                                            "NA"
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    5
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
                            "description": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:",
                            "details": "$_internal_rankFusion_internal_fields.calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedRankFusionDesugarerTest, ScoreDetailsSearchInputGeneratesScoreDetails) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    searchPipe: [
                        { $search: { index: "idx",
                                     text: { query: "x", path: "p" },
                                     scoreDetails: true } }
                    ]
                }
            },
            scoreDetails: true
        }
    })");
    BSONObj actual = desugar(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$search": {
                        "index": "idx",
                        "text": {
                            "query": "x",
                            "path": "p"
                        },
                        "scoreDetails": true
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
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
                        "_internal_rankFusion_internal_fields.searchPipe_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                                60
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
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.searchPipe_scoreDetails": {
                            "$meta": "scoreDetails"
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.searchPipe_score",
                                    0
                                ]
                            }
                        },
                        "__hs_searchPipe_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                    0
                                ]
                            }
                        },
                        "__hs_searchPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.searchPipe_scoreDetails"
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
                        "_internal_rankFusion_internal_fields.searchPipe_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.searchPipe_rank"
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
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
                                            "inputPipelineName": "searchPipe",
                                            "rank": "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.searchPipe_rank",
                                                            "NA"
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    1
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
                            "description": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:",
                            "details": "$_internal_rankFusion_internal_fields.calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedRankFusionDesugarerTest, MultipleSortStagesRightmostMutated) {
    // The leading $sort here should NOT receive $_internalOutputSortKeyMetadata; only the
    // rightmost $sort is mutated.
    unittest::ServerParameterGuard disableFull("featureFlagRankFusionFull", false);
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p: [
                        { $sort: { a: 1 } },
                        { $match: { b: 2 } },
                        { $sort: { c: 1 } }
                    ]
                }
            }
        }
    })");
    BSONObj actual = desugar(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$sort": {
                        "a": 1
                    }
                },
                {
                    "$match": {
                        "b": 2
                    }
                },
                {
                    "$sort": {
                        "c": 1,
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.p_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.p_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.p_rank",
                                                60
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
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_p_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.p_score",
                                    0
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
                                        "p_score": "$__hs_p_score"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.p_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.p_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.p_rank"
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.p_score"
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
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

// Covers the "input generates score but NOT scoreDetails" branch of
// `addInputPipelineScoreDetails`: the `$match` with `$text` publishes textScore metadata, the
// explicit `$sort` on `{$meta: "textScore"}` makes the pipeline ranked, and there is no
// scoreDetails-producing stage. Pair with top-level scoreDetails:true so the per-pipeline
// scoreDetails $addFields synthesizes the {value, details: []} form (the second branch).
TEST_F(LiteParsedRankFusionDesugarerTest, ScoreDetails_InputGeneratesScoreOnly) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    textPipe: [
                        { $match: { $text: { $search: "mystery" } } },
                        { $sort: { score: { $meta: "textScore" } } }
                    ]
                }
            },
            scoreDetails: true
        }
    })");
    BSONObj actual = desugar(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$match": {
                        "$text": {
                            "$search": "mystery"
                        }
                    }
                },
                {
                    "$sort": {
                        "score": {
                            "$meta": "textScore"
                        },
                        "$_internalOutputSortKeyMetadata": true
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_rankFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$_internalSetWindowFields": {
                        "sortBy": {
                            "order": 1
                        },
                        "output": {
                            "_internal_rankFusion_internal_fields.textPipe_rank": {
                                "$rank": {}
                            }
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.textPipe_score": {
                            "$multiply": [
                                {
                                    "$divide": [
                                        1,
                                        {
                                            "$add": [
                                                "$_internal_rankFusion_internal_fields.textPipe_rank",
                                                60
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
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.textPipe_scoreDetails": {
                            "value": {
                                "$meta": "score"
                            },
                            "details": []
                        }
                    }
                },
                {
                    "$group": {
                        "_id": "$_internal_rankFusion_docs._id",
                        "_internal_rankFusion_docs": {
                            "$first": "$_internal_rankFusion_docs"
                        },
                        "__hs_textPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.textPipe_score",
                                    0
                                ]
                            }
                        },
                        "__hs_textPipe_rank": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_rankFusion_internal_fields.textPipe_rank",
                                    0
                                ]
                            }
                        },
                        "__hs_textPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_rankFusion_internal_fields.textPipe_scoreDetails"
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
                                        "textPipe_score": "$__hs_textPipe_score",
                                        "textPipe_rank": "$__hs_textPipe_rank",
                                        "textPipe_scoreDetails": "$__hs_textPipe_scoreDetails"
                                    }
                                }
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_rankFusion_internal_fields.textPipe_rank": {
                            "$cond": [
                                {
                                    "$eq": [
                                        "$_internal_rankFusion_internal_fields.textPipe_rank",
                                        0
                                    ]
                                },
                                "NA",
                                "$_internal_rankFusion_internal_fields.textPipe_rank"
                            ]
                        }
                    }
                },
                {
                    "$setMetadata": {
                        "score": {
                            "$add": [
                                "$_internal_rankFusion_internal_fields.textPipe_score"
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
                                            "inputPipelineName": "textPipe",
                                            "rank": "$_internal_rankFusion_internal_fields.textPipe_rank",
                                            "weight": {
                                                "$cond": [
                                                    {
                                                        "$eq": [
                                                            "$_internal_rankFusion_internal_fields.textPipe_rank",
                                                            "NA"
                                                        ]
                                                    },
                                                    "$$REMOVE",
                                                    1
                                                ]
                                            }
                                        },
                                        "$_internal_rankFusion_internal_fields.textPipe_scoreDetails"
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
                            "description": "value output by reciprocal rank fusion algorithm, computed as sum of (weight * (1 / (60 + rank))) across input pipelines from which this document is output, from:",
                            "details": "$_internal_rankFusion_internal_fields.calculatedScoreDetails"
                        }
                    }
                },
                {
                    "$sort": {
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_rankFusion_internal_fields": 0
                    }
                },
                {
                    "$_internalHybridSearch": {}
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedRankFusionDesugarerTest, RejectsNegativeWeight) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    name1: [{ $sort: { x: 1 } }]
                }
            },
            combination: { weights: { name1: -1.0 } }
        }
    })");
    ASSERT_THROWS_CODE(desugar(spec), AssertionException, 12559401);
}

TEST_F(LiteParsedRankFusionDesugarerTest, RejectsNonNumericWeight) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    name1: [{ $sort: { x: 1 } }]
                }
            },
            combination: { weights: { name1: "not a number" } }
        }
    })");
    ASSERT_THROWS_CODE(desugar(spec), AssertionException, 12559404);
}

TEST_F(LiteParsedRankFusionDesugarerTest, RejectsUnknownPipelineNameInWeights) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    name1: [{ $sort: { x: 1 } }]
                }
            },
            combination: { weights: { typo: 1.0 } }
        }
    })");
    ASSERT_THROWS_CODE(desugar(spec), AssertionException, 9967500);
}

TEST_F(LiteParsedRankFusionDesugarerTest, RejectsMoreWeightsThanPipelines) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    name1: [{ $sort: { x: 1 } }]
                }
            },
            combination: { weights: { name1: 1.0, extraName: 1.0 } }
        }
    })");
    ASSERT_THROWS_CODE(desugar(spec), AssertionException, 12559403);
}

TEST_F(LiteParsedRankFusionDesugarerTest, DesugarContainsInternalHybridSearchLast) {
    BSONObj spec = fromjson(R"({
        $rankFusion: {
            input: {
                pipelines: {
                    p: [ { $sort: { a: 1 } } ]
                }
            }
        }
    })");
    const NamespaceString& nss = getExpCtx()->getNamespaceString();
    auto lprf = parseRankFusion(nss, spec);
    auto desugared =
        lite_parsed_hybrid_search_desugarer::desugarRankFusion(*lprf, nss, "pipeline_test"_sd);
    ASSERT_FALSE(desugared.empty());
    ASSERT_EQ(desugared.back()->getParseTimeName(), "$_internalHybridSearch");
}

}  // namespace
}  // namespace mongo
