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

#include <fmt/format.h>
#include <memory>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

class CommonSortKeyOptimizationTest : public AggregationContextFixture {
protected:
    void verify(const BSONObj& groupSpec, const std::vector<BSONObj>& expectedOptimizedPipeline) {
        using namespace fmt::literals;
        auto pipeline = Pipeline::parse(makeVector(groupSpec), getExpCtx());

        ASSERT_EQ(pipeline->getSources().size(), 1U);

        pipeline->optimizePipeline();

        auto actualOptimizedPipeline = pipeline->serializeToBson();
        ASSERT_EQ(actualOptimizedPipeline.size(), expectedOptimizedPipeline.size())
            << "Expected {} stages but got: "_format(expectedOptimizedPipeline.size())
            << to_string(actualOptimizedPipeline);

        for (size_t i = 0; i < actualOptimizedPipeline.size(); ++i) {
            ASSERT_BSONOBJ_EQ_UNORDERED(actualOptimizedPipeline[i], expectedOptimizedPipeline[i]);
        }
    }
};

TEST_F(CommonSortKeyOptimizationTest, MultipleTopsWithSameSortKeyOptimizedIntoOneTop) {
    const auto groupWithTwoTopsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        tm: {$top: {output: "$m", sortBy: {time: 1}}},
        ti: {$top: {output: "$i", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        ts: {
            $top: {
                output: {tm: "$m", ti: "$i"},
                sortBy: {time: 1}
            }
        }
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        tm: {$ifNull: ["$ts.tm", {$const: null}]},
        ti: {$ifNull: ["$ts.ti", {$const: null}]}
    }
}
    )");

    verify(groupWithTwoTopsWithSameSortPattern,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest, MultipleBottomNsWithSameSortKeySameNOptimizedIntoOneBottom) {
    const auto groupWithTwoBottomNsWithSameSortPatternSameN = fromjson(R"(
{
    $group: {
        _id: "$tag",
        bnm: {
            $bottomN: {
                n: {$cond: {if: {$eq: ["$tag", "WA"]}, then: 10, else: 4}},
                output: "$m",
                sortBy: {time: 1}
            }
        },
        bni: {
            $bottomN: {
                n: {$cond: {if: {$eq: ["$tag", "WA"]}, then: 10, else: 4}},
                output: "$i",
                sortBy: {time: 1}
            }
        }
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: "$tag",
        bns: {
            $bottomN: {
                n: {$cond: [{$eq: ["$tag", {$const: "WA"}]}, {$const: 10}, {$const: 4}]},
                output: {bnm: "$m", bni: "$i"},
                sortBy: {time: 1}
            }
        }
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        bnm: {$ifNull: ["$bns.bnm", {$const: null}]},
        bni: {$ifNull: ["$bns.bni", {$const: null}]}
    }
}
    )");

    verify(groupWithTwoBottomNsWithSameSortPatternSameN,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest, MultipleTopNsWithSameSortKeyOptimizedIntoOneTopN) {
    const auto groupWithTwoTopNsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        tm: {$topN: {n: 2, output: "$m", sortBy: {time: 1}}},
        ti: {$topN: {n: 2, output: "$i", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        tns: {
            $topN: {
                n: {$const: 2},
                output: {tm: "$m", ti: "$i"},
                sortBy: {time: 1}
            }
        }
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        tm: {$ifNull: ["$tns.tm", {$const: null}]},
        ti: {$ifNull: ["$tns.ti", {$const: null}]}
    }
}
    )");

    verify(groupWithTwoTopNsWithSameSortPattern,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest, MultipleBottomsWithSameSortKeyOptimizedIntoOneBottom) {
    const auto groupWithTwoBottomsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        bm: {$bottom: {output: "$m", sortBy: {time: 1}}},
        bi: {$bottom: {output: "$i", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        bs: {
            $bottom: {
                output: {bm: "$m", bi: "$i"},
                sortBy: {time: 1}
            }
        }
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        bm: {$ifNull: ["$bs.bm", {$const: null}]},
        bi: {$ifNull: ["$bs.bi", {$const: null}]}
    }
}
    )");
    verify(groupWithTwoBottomsWithSameSortPattern,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest, MultipleBottomNsWithSameSortKeyOptimizedIntoOneBottomN) {
    const auto groupWithTwoBottomNsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        bm: {$bottomN: {n: 3, output: "$m", sortBy: {time: 1}}},
        bi: {$bottomN: {n: 3, output: "$i", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        bns: {
            $bottomN: {
                n: {$const: 3},
                output: {bm: "$m", bi: "$i"},
                sortBy: {time: 1}
            }
        }
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        bm: {$ifNull: ["$bns.bm", {$const: null}]},
        bi: {$ifNull: ["$bns.bi", {$const: null}]}
    }
}
    )");
    verify(groupWithTwoBottomNsWithSameSortPattern,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest,
       MultipleTopBottomsWithSameSortKeyOptimizedIntoMultipleTopBottoms) {
    const auto groupWithMultipleTopBottomsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        t_a: {$top: {output: "$a", sortBy: {time: 1, g: -1}}},
        t_b: {$top: {output: "$b", sortBy: {time: 1, g: -1}}},
        t_c: {$top: {output: "$c", sortBy: {time: 1, g: -1}}},
        b_a: {$bottomN: {n: 2, output: "$a", sortBy: {time: 1, g: 1}}},
        b_b: {$bottomN: {n: 2, output: "$b", sortBy: {time: 1, g: 1}}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        ts: {
            $top: {
                output: {t_a: "$a", t_b: "$b", t_c: "$c"},
                sortBy: {time: 1, g: -1}
            }
        },
        bns: {
            $bottomN: {
                n: {$const: 2},
                output: {b_a: "$a", b_b: "$b"},
                sortBy: {time: 1, g: 1}
            }
        }
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        t_a: {$ifNull: ["$ts.t_a", {$const: null}]},
        t_b: {$ifNull: ["$ts.t_b", {$const: null}]},
        t_c: {$ifNull: ["$ts.t_c", {$const: null}]},
        b_a: {$ifNull: ["$bns.b_a", {$const: null}]},
        b_b: {$ifNull: ["$bns.b_b", {$const: null}]}
    }
}
    )");
    verify(groupWithMultipleTopBottomsWithSameSortPattern,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest, MultiTopBottomsWithDifferentSortPatternNotOptimized) {
    // The accumulators of the same type cannot be optimized if the sort pattern is not same.
    const auto groupWithMultiTopsWithDifferentSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        t_a: {$top: {output: "$a", sortBy: {time: 1, g: -1}}},
        t_b: {$top: {output: "$b", sortBy: {time: 1, g: 1}}}
    }
}
    )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        t_a: {$top: {output: "$a", sortBy: {time: 1, g: -1}}},
        t_b: {$top: {output: "$b", sortBy: {time: 1, g: 1}}}
    }
}
    )");
    verify(groupWithMultiTopsWithDifferentSortPattern, makeVector(expectedNotOptimizedGroup));
}

TEST_F(CommonSortKeyOptimizationTest, DifferentAccumulatorsWithSameSortPatternNotOptimized) {
    // The different accumulator types cannot be optimized though they have the same sort pattern.
    const auto groupWithDifferentAccumulatorsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        t_a: {$top: {output: "$a", sortBy: {time: 1}}},
        b_b: {$bottom: {output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        t_a: {$top: {output: "$a", sortBy: {time: 1}}},
        b_b: {$bottom: {output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    verify(groupWithDifferentAccumulatorsWithSameSortPattern,
           makeVector(expectedNotOptimizedGroup));
}

TEST_F(CommonSortKeyOptimizationTest, OneTopAndOneTopNWithSameSortPatternNotOptimized) {
    // The different accumulator types cannot be optimized though they have the same sort pattern.
    const auto groupWithDifferentAccumulatorsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        t_a: {$top: {output: "$a", sortBy: {time: 1}}},
        t_b: {$topN: {n: 1, output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        t_a: {$top: {output: "$a", sortBy: {time: 1}}},
        t_b: {$topN: {n: {$const: 1}, output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    verify(groupWithDifferentAccumulatorsWithSameSortPattern,
           makeVector(expectedNotOptimizedGroup));
}

TEST_F(CommonSortKeyOptimizationTest, DifferentNsForBottomNsWithSameSortPatternNotOptimized) {
    // There are two $bottomN but they don't have the same 'n' arguments.
    const auto groupWithDifferentNBottomNsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        b_a: {$bottomN: {n: 2, output: "$a", sortBy: {time: 1}}},
        b_b: {$bottomN: {n: 3, output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        b_a: {$bottomN: {n: {$const: 2}, output: "$a", sortBy: {time: 1}}},
        b_b: {$bottomN: {n: {$const: 3}, output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    verify(groupWithDifferentNBottomNsWithSameSortPattern, makeVector(expectedNotOptimizedGroup));
}

TEST_F(CommonSortKeyOptimizationTest, OptimizableTopNsMixedWithIneligibleAccumulators) {
    // $first and $last are not eligible for this optimization.
    const auto groupWithOptimizableTopNsMixedWithFirstLast = fromjson(R"(
{
    $group: {
        _id: null,
        t_a: {$topN: {n: 5, output: "$a", sortBy: {time: 1, g: -1}}},
        t_b: {$topN: {n: 5, output: "$b", sortBy: {time: 1, g: -1}}},
        fc: {$first: "$c"},
        ld: {$last: "$d"}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        tns: {
            $topN: {
                n: {$const: 5},
                output: {t_a: "$a", t_b: "$b"},
                sortBy: {time: 1, g: -1}
            }
        },
        fc: {$first: "$c"},
        ld: {$last: "$d"}
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        t_a: {$ifNull: ["$tns.t_a", {$const: null}]},
        t_b: {$ifNull: ["$tns.t_b", {$const: null}]},
        fc: true,
        ld: true
    }
}
    )");
    verify(groupWithOptimizableTopNsMixedWithFirstLast,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest,
       OptimizableTopNsMixedWithIneligibleAndNotOptimizableAccumulators) {
    // The $bottom is eligible for the optimization but there's only one $bottom and so it's not
    // optimized.
    const auto groupWithOptimizableTopNsMixedWithFirstLastOneBottom = fromjson(R"(
{
    $group: {
        _id: null,
        b_k: {$bottom: {output: "$k", sortBy: {time: 1, g: -1}}},
        t_a: {$topN: {n: 5, output: "$a", sortBy: {time: 1, g: -1}}},
        t_b: {$topN: {n: 5, output: "$b", sortBy: {time: 1, g: -1}}},
        fc: {$first: "$c"},
        ld: {$last: "$d"}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        b_k: {$bottom: {output: "$k", sortBy: {time: 1, g: -1}}},
        tns: {
            $topN: {
                n: {$const: 5},
                output: {t_a: "$a", t_b: "$b"},
                sortBy: {time: 1, g: -1}
            }
        },
        fc: {$first: "$c"},
        ld: {$last: "$d"}
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        b_k: true,
        t_a: {$ifNull: ["$tns.t_a", {$const: null}]},
        t_b: {$ifNull: ["$tns.t_b", {$const: null}]},
        fc: true,
        ld: true
    }
}
    )");
    verify(groupWithOptimizableTopNsMixedWithFirstLastOneBottom,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}
}  // namespace
}  // namespace mongo
