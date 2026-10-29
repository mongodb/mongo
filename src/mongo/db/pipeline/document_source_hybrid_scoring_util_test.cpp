// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

TEST(BsonStageIsSelectionStage, LimitIsSelection) {
    const auto& stageBson = fromjson(R"({
            $limit: 5
        })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, MatchIsSelection) {
    const auto& stageBson = fromjson(R"({
            $match: {
                _id: {
                    $eq: 1
                }
            }
        })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, RankFusionIsSelection) {
    const auto& stageBson = fromjson(R"({
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
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, SampleIsSelection) {
    const auto& stageBson = fromjson(R"({
        "$sample": {
            "size": 5
        }
    })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, ScoreFusionIsSelection) {
    const auto& stageBson = fromjson(R"({
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
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, SkipIsSelection) {
    const auto& stageBson = fromjson(R"({
        $skip: 5
    })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, SortIsSelection) {
    const auto& stageBson = fromjson(R"({
        $sort: {
            _id: 1
        }
    })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, NotAllowedStageNotSelection) {
    const auto& stageBson = fromjson(R"({
            $project: {
                _id: 1
            }
        })");
    ASSERT_FALSE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, VectorSearchIsSelection) {
    const auto& stageBson = fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10
        }
    })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(stageBson).isOK());
}

TEST(BsonStageIsSelectionStage, GeoNear) {
    const auto& selectionGeoNear = fromjson("{$geoNear: {near: [0, 0]}}");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(selectionGeoNear).isOK());

    // Invalid syntax marked as not selection.
    const auto& geoNearNotAnObject = fromjson("{$geoNear: 5}");
    ASSERT_FALSE(hybrid_scoring_util::isSelectionStage(geoNearNotAnObject).isOK());

    // includeLocs marked as not selection.
    const auto& geoNearIncludeLocs = fromjson(R"({
        $geoNear: {
            includeLocs: "bar.baz",
            near: [10, 10],
            key: "z",
            query: {
                a : { $gt: 10 }
            },
            spherical: false
        }
    })");
    ASSERT_FALSE(hybrid_scoring_util::isSelectionStage(geoNearIncludeLocs).isOK());

    // distanceField marked as not selection.
    auto geoNearDistanceField = fromjson(R"({
        $geoNear: {
            distanceField: "foo",
            distanceMultiplier: 3.14,
            near: [10, 10],
            key: "z",
            query: {
                a : { $gt: 10 }
            },
            spherical: false
        }
    })");
    ASSERT_FALSE(hybrid_scoring_util::isSelectionStage(geoNearDistanceField).isOK());
}

TEST(BsonStageIsSelectionStage, Search) {
    const auto& search = fromjson(R"({
            $search: {
                term: "asdf"
            }
        })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(search).isOK());

    // Invalid syntax marked as not selection.
    const auto& searchNotAnObject = fromjson(R"({
            $search: 5
        })");
    ASSERT_FALSE(hybrid_scoring_util::isSelectionStage(searchNotAnObject).isOK());

    // Explicit 'returnStoredSource' false marked as selection
    const auto& explicitReturnStoredSourceFalse = fromjson(R"({
                $search: {
                    index: "search_index",
                    text: {
                        query: "mystery",
                        path: "genres"
                    },
                    "returnStoredSource": false
                }
            })");
    ASSERT_TRUE(hybrid_scoring_util::isSelectionStage(explicitReturnStoredSourceFalse).isOK());

    // Explicit 'returnStoredSource' true marked as not selection
    const auto& explicitReturnStoredSourceTrue = fromjson(R"({
                $search: {
                    index: "search_index",
                    text: {
                        query: "mystery",
                        path: "genres"
                    },
                    "returnStoredSource": true
                }
            })");
    ASSERT_FALSE(hybrid_scoring_util::isSelectionStage(explicitReturnStoredSourceTrue).isOK());

    // 'returnStoredSource' not boolean marked as not selection
    const auto& returnStoredSourceNotBoolean = fromjson(R"({
                $search: {
                    index: "search_index",
                    text: {
                        query: "mystery",
                        path: "genres"
                    },
                    "returnStoredSource": 5
                }
            })");
    ASSERT_FALSE(hybrid_scoring_util::isSelectionStage(returnStoredSourceNotBoolean).isOK());
}

TEST(BsonPipelineisOrderedPipline, MustBeginWithOrderedStageOrHaveSort) {
    std::vector<BSONObj> geoNearPipeline = {fromjson(R"({
            $geoNear: {
                distanceField: "foo",
                distanceMultiplier: 3.14,
                near: [10, 10],
                key: "z",
                query: {
                    a : { $gt: 10 }
                },
                spherical: false
            }
        })"),
                                            fromjson(R"({
            "$sample": {
                "size": 5
            }
        })")};
    ASSERT_TRUE(hybrid_scoring_util::isRankedPipeline(geoNearPipeline).isOK());

    std::vector<BSONObj> searchPipeline = {fromjson(R"({
            $search: {
                index: "search_index",
                text: {
                    query: "mystery",
                    path: "genres"
                },
                "returnStoredSource": false
            }
        })"),
                                           fromjson(R"({
            "$sample": {
                "size": 5
            }
        })")};
    ASSERT_TRUE(hybrid_scoring_util::isRankedPipeline(searchPipeline).isOK());

    std::vector<BSONObj> vectorSearchPipeline = {fromjson(R"({
            $vectorSearch: {
                queryVector: [1.0, 2.0],
                path: "x",
                numCandidates: 100,
                limit: 10
            }
        })"),
                                                 fromjson(R"({
            "$sample": {
                "size": 5
            }
        })")};
    ASSERT_TRUE(hybrid_scoring_util::isRankedPipeline(vectorSearchPipeline).isOK());

    ASSERT_FALSE(hybrid_scoring_util::isRankedPipeline({}).isOK());

    std::vector<BSONObj> orderedNotFirstPipeline = {fromjson(R"({
            "$sample": {
                "size": 5
            }
        })"),
                                                    fromjson(R"({
            $vectorSearch: {
                queryVector: [1.0, 2.0],
                path: "x",
                numCandidates: 100,
                limit: 10
            }
        })")};
    ASSERT_FALSE(hybrid_scoring_util::isRankedPipeline(orderedNotFirstPipeline).isOK());

    std::vector<BSONObj> noOrderedStages = {fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_FALSE(hybrid_scoring_util::isRankedPipeline(noOrderedStages).isOK());

    std::vector<BSONObj> sortStage = {fromjson(R"({
        "$sample": {
            "size": 5
        }
    })"),
                                      fromjson(R"({
        "$sort": {
            "_id": 1
        }
    })"),
                                      fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isRankedPipeline(sortStage).isOK());
}

using BsonPipelineIsScoredPipelineTest = AggregationContextFixture;

TEST_F(BsonPipelineIsScoredPipelineTest, MustBeginWithScoredStageOrHaveScore) {
    std::vector<BSONObj> searchPipeline = {fromjson(R"({
        $search: {
            index: "search_index",
            text: {
                query: "mystery",
                path: "genres"
            },
            "returnStoredSource": false
        }
    })"),
                                           fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isScoredPipeline(searchPipeline, getExpCtx()).isOK());

    std::vector<BSONObj> vectorSearchPipeline = {fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10
        }
    })"),
                                                 fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isScoredPipeline(vectorSearchPipeline, getExpCtx()).isOK());

    std::vector<BSONObj> matchTextPipeline = {fromjson(R"({
        $match: {
            $text: {
                $search: "apples pears"
            }
        }
    })"),
                                              fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isScoredPipeline(matchTextPipeline, getExpCtx()).isOK());

    std::vector<BSONObj> matchNoTextPipeline = {fromjson(R"({
        $match: {
            _id: {
                $in: [0, 1, 2]
            }
        }
    })"),
                                                fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_FALSE(hybrid_scoring_util::isScoredPipeline(matchNoTextPipeline, getExpCtx()).isOK());

    ASSERT_FALSE(hybrid_scoring_util::isScoredPipeline({}, getExpCtx()).isOK());

    std::vector<BSONObj> scoredNotFirstPipeline = {fromjson(R"({
        "$sample": {
            "size": 5
        }
    })"),
                                                   fromjson(R"({
        $vectorSearch: {
            queryVector: [1.0, 2.0],
            path: "x",
            numCandidates: 100,
            limit: 10
        }
    })")};
    ASSERT_FALSE(hybrid_scoring_util::isScoredPipeline(scoredNotFirstPipeline, getExpCtx()).isOK());

    std::vector<BSONObj> noScoredStages = {fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_FALSE(hybrid_scoring_util::isScoredPipeline(noScoredStages, getExpCtx()).isOK());

    std::vector<BSONObj> scoreStage = {fromjson(R"({
        "$sample": {
            "size": 5
        }
    })"),
                                       fromjson(R"({
        "$score": {
            "score": 1
        }
    })"),
                                       fromjson(R"({
        "$sample": {
            "size": 5
        }
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isScoredPipeline(scoreStage, getExpCtx()).isOK());
}

TEST(BsonPipelineIsHybridSearchPipeline, ScoreFusion) {
    std::vector<BSONObj> scoreFusionFirstPipeline = {fromjson(R"({
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
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isHybridSearchPipeline(scoreFusionFirstPipeline));

    std::vector<BSONObj> scoreFusionSecondPipeline = {fromjson(R"({
            $match: {a: {gte: 5}}
        })"),
                                                      fromjson(R"({
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
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isHybridSearchPipeline(scoreFusionSecondPipeline));
}

TEST(BsonPipelineIsHybridSearchPipeline, RankFusion) {
    std::vector<BSONObj> rankFusionFirstPipeline = {fromjson(R"({
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
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isHybridSearchPipeline(rankFusionFirstPipeline));

    std::vector<BSONObj> rankFusionSecondPipeline = {fromjson(R"({
            $match: {a: {gte: 5}}
        })"),
                                                     fromjson(R"({
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
    })")};
    ASSERT_TRUE(hybrid_scoring_util::isHybridSearchPipeline(rankFusionSecondPipeline));
}

TEST(BsonPipelineIsHybridSearchPipeline, NonHybridSearchStages) {
    std::vector<BSONObj> otherStagesPipeline = {fromjson(R"(
        {
            $match: {a: {gte: 5}}
        })"),
                                                fromjson(R"({
            $limit: 10
        })"),
                                                fromjson(R"({
            $sort: {a: -1}
    })")};
    ASSERT_FALSE(hybrid_scoring_util::isHybridSearchPipeline(otherStagesPipeline));
}

TEST(BsonPipelineContainsScoreStage, PipelineContainsScoreStage) {
    std::vector<BSONObj> noScoreStagePipeline = {fromjson(R"(
        {
            $match: {a: {gte: 5}}
        })"),
                                                 fromjson(R"({
            $limit: 10
        })"),
                                                 fromjson(R"({
            $sort: {a: -1}
    })")};
    ASSERT_FALSE(hybrid_scoring_util::pipelineContainsScoreStage(noScoreStagePipeline));

    std::vector<BSONObj> scoreStagePipeline = {fromjson(R"(
        {
            $match: {a: {gte: 5}}
        })"),
                                               fromjson(R"({
            $score: {score: 10}
        })"),
                                               fromjson(R"({
            $sort: {a: -1}
    })")};
    ASSERT_TRUE(hybrid_scoring_util::pipelineContainsScoreStage(scoreStagePipeline));

    std::vector<BSONObj> scoreStageFirstPipeline = {fromjson(R"({
            $score: {score: 10}
        })"),
                                                    fromjson(R"(
        {
            $match: {a: {gte: 5}}
        })"),
                                                    fromjson(R"({
            $sort: {a: -1}
    })")};
    ASSERT_TRUE(hybrid_scoring_util::pipelineContainsScoreStage(scoreStageFirstPipeline));
}

}  // namespace
}  // namespace mongo
