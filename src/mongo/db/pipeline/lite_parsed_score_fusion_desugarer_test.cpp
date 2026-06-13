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
#include "mongo/db/pipeline/document_source_score_fusion.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_hybrid_search_desugarer.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/lite_parsed_score_fusion.h"
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

// Parse the top-level pipeline (a vector of one stage) into a LiteParsedScoreFusion to
// drive the desugarer.
std::unique_ptr<LiteParsedScoreFusion> parseScoreFusion(const NamespaceString& nss,
                                                        const BSONObj& specObj) {
    BSONObj ownedSpec = specObj.getOwned();
    auto lpds = LiteParsedDocumentSource::parse(nss, ownedSpec);
    lpds->makeOwned();
    auto* sfPtr = dynamic_cast<LiteParsedScoreFusion*>(lpds.get());
    invariant(sfPtr);
    lpds.release();
    return std::unique_ptr<LiteParsedScoreFusion>(sfPtr);
}

class LiteParsedScoreFusionDesugarerTest : public service_context_test::WithSetupTransportLayer,
                                           public AggregationContextFixture {
protected:
    BSONObj desugarScore(const BSONObj& scoreFusionSpecObj) {
        const NamespaceString& nss = getExpCtx()->getNamespaceString();
        auto lpsf = parseScoreFusion(nss, scoreFusionSpecObj);
        auto desugared =
            lite_parsed_hybrid_search_desugarer::desugarScoreFusion(*lpsf, nss, "pipeline_test"_sd);
        return toExpectedStagesBson(desugared);
    }

private:
    unittest::ServerParameterGuard featureFlagController1{"featureFlagSearchHybridScoringFull",
                                                          true};
};

TEST_F(LiteParsedScoreFusionDesugarerTest, SinglePipelineDefault) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                1
                            ]
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, SinglePipelineSigmoid) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ]
                },
                normalization: "sigmoid"
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$sigmoid": {
                                        "$meta": "score"
                                    }
                                },
                                1
                            ]
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, SinglePipelineMinMaxScaler) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ]
                },
                normalization: "minMaxScaler"
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                1
                            ]
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
                                    "input": "$_internal_scoreFusion_internal_fields.name1_score"
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, MultiplePipelinesMixedNone) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
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
                    ],
                    scoredMatch: [
                        { $match: { author: "Agatha Christie" } },
                        { $score: { score: "$age", normalization: "none" } }
                    ]
                },
                normalization: "none"
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
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
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.matchGenres_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
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
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.matchPlot_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
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
                    "$unionWith": {
                        "coll": "pipeline_test",
                        "pipeline": [
                            {
                                "$match": {
                                    "author": "Agatha Christie"
                                }
                            },
                            {
                                "$score": {
                                    "score": "$age",
                                    "normalization": "none"
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.scoredMatch_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_matchGenres_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchGenres_score",
                                    0
                                ]
                            }
                        },
                        "__hs_matchPlot_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.matchPlot_score",
                                    0
                                ]
                            }
                        },
                        "__hs_scoredMatch_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.scoredMatch_score",
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
                                "$_internal_scoreFusion_docs",
                                {
                                    "_internal_scoreFusion_internal_fields": {
                                        "matchGenres_score": "$__hs_matchGenres_score",
                                        "matchPlot_score": "$__hs_matchPlot_score",
                                        "scoredMatch_score": "$__hs_scoredMatch_score"
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
                                "$_internal_scoreFusion_internal_fields.matchGenres_score",
                                "$_internal_scoreFusion_internal_fields.matchPlot_score",
                                "$_internal_scoreFusion_internal_fields.scoredMatch_score"
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
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, MultiplePipelinesSigmoid) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    name2: [
                        { $score: { score: "$rating", normalization: "none" } }
                    ]
                },
                normalization: "sigmoid"
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$sigmoid": {
                                        "$meta": "score"
                                    }
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
                                "$score": {
                                    "score": "$rating",
                                    "normalization": "none"
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_score": {
                                        "$multiply": [
                                            {
                                                "$sigmoid": {
                                                    "$meta": "score"
                                                }
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    0
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, MultiplePipelinesMinMaxScaler) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    name2: [
                        { $score: { score: "$rating", normalization: "none" } }
                    ]
                },
                normalization: "minMaxScaler"
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                1
                            ]
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
                                    "input": "$_internal_scoreFusion_internal_fields.name1_score"
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
                                "$score": {
                                    "score": "$rating",
                                    "normalization": "none"
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            1
                                        ]
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
                                                "input": "$_internal_scoreFusion_internal_fields.name2_score"
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
                                    0
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, CombinationMethodAvgExplicit) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    name2: [
                        { $score: { score: "$rating", normalization: "none" } }
                    ]
                },
                normalization: "none"
            },
            combination: { method: "avg" }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
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
                                "$score": {
                                    "score": "$rating",
                                    "normalization": "none"
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    0
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, CombinationMethodExpression) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    name2: [
                        { $score: { score: "$rating", normalization: "none" } }
                    ]
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: { $sum: ["$$name1", "$$name2", 5.0] }
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
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
                                "$score": {
                                    "score": "$rating",
                                    "normalization": "none"
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
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
                        "_id": "$_internal_scoreFusion_docs._id",
                        "_internal_scoreFusion_docs": {
                            "$first": "$_internal_scoreFusion_docs"
                        },
                        "__hs_name1_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_score",
                                    0
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                                        5
                                    ]
                                }
                            }
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
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, CustomWeights) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    name2: [
                        { $score: { score: "$rating", normalization: "none" } }
                    ]
                },
                normalization: "none"
            },
            combination: {
                weights: { name1: 5, name2: 2 }
            }
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
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
                                "$score": {
                                    "score": "$rating",
                                    "normalization": "none"
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            2
                                        ]
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
                                    0
                                ]
                            }
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
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
                        "score": {
                            "$meta": "score"
                        },
                        "_id": 1
                    }
                },
                {
                    "$project": {
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, ScoreDetailsScoreInputGeneratesScoreOnly) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ]
                },
                normalization: "none"
            },
            combination: { weights: { name1: 5 } },
            scoreDetails: true
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                5
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_scoreDetails": {
                            "details": []
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
                                    0
                                ]
                            }
                        },
                        "__hs_name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                    0
                                ]
                            }
                        },
                        "__hs_name1_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
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
                                            "inputPipelineName": "name1",
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                            "weight": 5,
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
                            "description": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:",
                            "normalization": "none",
                            "combination": {
                                "method": "average"
                            },
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, ScoreDetailsSearchGeneratesScoreDetails) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    searchPipe: [
                        { $search: { index: "idx",
                                     text: { query: "x", path: "p" },
                                     scoreDetails: true } }
                    ]
                },
                normalization: "none"
            },
            scoreDetails: true
        }
    })");
    BSONObj actual = desugarScore(spec);
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
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.searchPipe_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                1
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.searchPipe_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.searchPipe_scoreDetails": {
                            "details": {
                                "$meta": "scoreDetails"
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
                        "__hs_searchPipe_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.searchPipe_score",
                                    0
                                ]
                            }
                        },
                        "__hs_searchPipe_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.searchPipe_rawScore",
                                    0
                                ]
                            }
                        },
                        "__hs_searchPipe_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.searchPipe_scoreDetails"
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
                                            "inputPipelineName": "searchPipe",
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.searchPipe_rawScore",
                                            "weight": 1,
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
                            "description": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:",
                            "normalization": "none",
                            "combination": {
                                "method": "average"
                            },
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, ScoreDetailsExpressionCombination) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [
                        { $score: { score: "$age", normalization: "none" } }
                    ],
                    name2: [
                        { $score: { score: "$rating", normalization: "none" } }
                    ]
                },
                normalization: "minMaxScaler"
            },
            combination: {
                method: "expression",
                expression: { $sum: ["$$name1", "$$name2", 5.0] }
            },
            scoreDetails: true
        }
    })");
    BSONObj actual = desugarScore(spec);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "expectedStages": [
                {
                    "$score": {
                        "score": "$age",
                        "normalization": "none"
                    }
                },
                {
                    "$replaceWith": {
                        "_internal_scoreFusion_docs": "$$ROOT"
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_score": {
                            "$multiply": [
                                {
                                    "$meta": "score"
                                },
                                1
                            ]
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_rawScore": {
                            "$meta": "score"
                        }
                    }
                },
                {
                    "$addFields": {
                        "_internal_scoreFusion_internal_fields.name1_scoreDetails": {
                            "details": []
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
                                    "input": "$_internal_scoreFusion_internal_fields.name1_score"
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
                                "$score": {
                                    "score": "$rating",
                                    "normalization": "none"
                                }
                            },
                            {
                                "$replaceWith": {
                                    "_internal_scoreFusion_docs": "$$ROOT"
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_score": {
                                        "$multiply": [
                                            {
                                                "$meta": "score"
                                            },
                                            1
                                        ]
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_rawScore": {
                                        "$meta": "score"
                                    }
                                }
                            },
                            {
                                "$addFields": {
                                    "_internal_scoreFusion_internal_fields.name2_scoreDetails": {
                                        "details": []
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
                                                "input": "$_internal_scoreFusion_internal_fields.name2_score"
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
                                    0
                                ]
                            }
                        },
                        "__hs_name1_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                    0
                                ]
                            }
                        },
                        "__hs_name1_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                        },
                        "__hs_name2_score": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_score",
                                    0
                                ]
                            }
                        },
                        "__hs_name2_rawScore": {
                            "$max": {
                                "$ifNull": [
                                    "$_internal_scoreFusion_internal_fields.name2_rawScore",
                                    0
                                ]
                            }
                        },
                        "__hs_name2_scoreDetails": {
                            "$mergeObjects": "$_internal_scoreFusion_internal_fields.name2_scoreDetails"
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
                                        "name1_score": "$__hs_name1_score",
                                        "name1_rawScore": "$__hs_name1_rawScore",
                                        "name1_scoreDetails": "$__hs_name1_scoreDetails",
                                        "name2_score": "$__hs_name2_score",
                                        "name2_rawScore": "$__hs_name2_rawScore",
                                        "name2_scoreDetails": "$__hs_name2_scoreDetails"
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
                                        5
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
                                            "inputPipelineName": "name1",
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.name1_rawScore",
                                            "weight": 1,
                                            "value": "$_internal_scoreFusion_internal_fields.name1_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.name1_scoreDetails"
                                    ]
                                },
                                {
                                    "$mergeObjects": [
                                        {
                                            "inputPipelineName": "name2",
                                            "inputPipelineRawScore": "$_internal_scoreFusion_internal_fields.name2_rawScore",
                                            "weight": 1,
                                            "value": "$_internal_scoreFusion_internal_fields.name2_score"
                                        },
                                        "$_internal_scoreFusion_internal_fields.name2_scoreDetails"
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
                            "description": "the value calculated by combining the scores (either normalized or raw) across input pipelines from which this document is output from:",
                            "normalization": "minMaxScaler",
                            "combination": {
                                "method": "custom expression",
                                "expression": "{ string: { $sum: [ '$$name1', '$$name2', 5.0 ] } }"
                            },
                            "details": "$_internal_scoreFusion_internal_fields.calculatedScoreDetails"
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
                        "_internal_scoreFusion_internal_fields": 0
                    }
                }
            ]
        })",
        actual);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, RejectsNegativeWeight) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [{ $score: { score: "$age", normalization: "none" } }]
                },
                normalization: "none"
            },
            combination: { weights: { name1: -1.0 } }
        }
    })");
    ASSERT_THROWS_CODE(desugarScore(spec), AssertionException, 12559401);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, RejectsNonNumericWeight) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [{ $score: { score: "$age", normalization: "none" } }]
                },
                normalization: "none"
            },
            combination: { weights: { name1: "not a number" } }
        }
    })");
    ASSERT_THROWS_CODE(desugarScore(spec), AssertionException, 12559404);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, RejectsUnknownPipelineNameInWeights) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [{ $score: { score: "$age", normalization: "none" } }]
                },
                normalization: "none"
            },
            combination: { weights: { typo: 1.0 } }
        }
    })");
    ASSERT_THROWS_CODE(desugarScore(spec), AssertionException, 12559400);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, RejectsMoreWeightsThanPipelines) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [{ $score: { score: "$age", normalization: "none" } }]
                },
                normalization: "none"
            },
            combination: { weights: { name1: 1.0, extraName: 1.0 } }
        }
    })");
    ASSERT_THROWS_CODE(desugarScore(spec), AssertionException, 12559403);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, RejectsExpressionWithoutExpressionMethod) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [{ $score: { score: "$age", normalization: "none" } }]
                },
                normalization: "none"
            },
            combination: { method: "avg", expression: { $sum: ["$$name1", 1] } }
        }
    })");
    ASSERT_THROWS_CODE(desugarScore(spec), AssertionException, 12559406);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, RejectsExpressionMethodWithoutExpression) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [{ $score: { score: "$age", normalization: "none" } }]
                },
                normalization: "none"
            },
            combination: { method: "expression" }
        }
    })");
    ASSERT_THROWS_CODE(desugarScore(spec), AssertionException, 12559406);
}

TEST_F(LiteParsedScoreFusionDesugarerTest, RejectsBothExpressionAndWeights) {
    BSONObj spec = fromjson(R"({
        $scoreFusion: {
            input: {
                pipelines: {
                    name1: [{ $score: { score: "$age", normalization: "none" } }]
                },
                normalization: "none"
            },
            combination: {
                method: "expression",
                expression: { $sum: ["$$name1", 1] },
                weights: { name1: 1.0 }
            }
        }
    })");
    ASSERT_THROWS_CODE(desugarScore(spec), AssertionException, 12559407);
}

}  // namespace
}  // namespace mongo
