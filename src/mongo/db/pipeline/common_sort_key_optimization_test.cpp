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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace {

class CommonSortKeyOptimizationTest : public AggregationContextFixture {
protected:
    void verify(const BSONObj& groupSpec, const std::vector<BSONObj>& expectedOptimizedPipeline) {
        auto pipeline = Pipeline::parse(makeVector(groupSpec), getExpCtx());

        ASSERT_EQ(pipeline->size(), 1U);

        pipeline->optimizePipeline();

        auto actualOptimizedPipeline = pipeline->serializeToBson();
        ASSERT_EQ(actualOptimizedPipeline.size(), expectedOptimizedPipeline.size())
            << "Expected " << expectedOptimizedPipeline.size() << " stages but got the following "
            << actualOptimizedPipeline.size() << " stages: " << to_string(actualOptimizedPipeline);

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
        ts_0: {
            $top: {
                output: {tm: {$ifNull: ["$m", {$const: null}]}, ti: {$ifNull: ["$i", {$const: null}]}},
                sortBy: {time: 1}
            }
        },
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        tm: "$ts_0.tm",
        ti: "$ts_0.ti"
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
        bns_0: {
            $bottomN: {
                n: {$cond: [{$eq: ["$tag", {$const: "WA"}]}, {$const: 10}, {$const: 4}]},
                output: {bnm: {$ifNull: ["$m", {$const: null}]}, bni: {$ifNull: ["$i", {$const: null}]}},
                sortBy: {time: 1}
            }
        },
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        bnm: "$bns_0.bnm",
        bni: "$bns_0.bni"
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
        tns_0: {
            $topN: {
                n: {$const: 2},
                output: {tm: {$ifNull: ["$m", {$const: null}]}, ti: {$ifNull: ["$i", {$const: null}]}},
                sortBy: {time: 1}
            }
        },
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        tm: "$tns_0.tm",
        ti: "$tns_0.ti"
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
        bs_0: {
            $bottom: {
                output: {bm: {$ifNull: ["$m", {$const: null}]}, bi: {$ifNull: ["$i", {$const: null}]}},
                sortBy: {time: 1}
            }
        },
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        bm: "$bs_0.bm",
        bi: "$bs_0.bi"
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
        bns_0: {
            $bottomN: {
                n: {$const: 3},
                output: {bm: {$ifNull: ["$m", {$const: null}]}, bi: {$ifNull: ["$i", {$const: null}]}},
                sortBy: {time: 1}
            }
        },
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        bm: "$bns_0.bm",
        bi: "$bns_0.bi"
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
        ts_0: {
            $top: {
                output: {t_a: {$ifNull: ["$a", {$const: null}]}, t_b: {$ifNull: ["$b", {$const: null}]}, t_c: {$ifNull: ["$c", {$const: null}]}},
                sortBy: {time: 1, g: -1}
            }
        },
        bns_3: {
            $bottomN: {
                n: {$const: 2},
                output: {b_a: {$ifNull: ["$a", {$const: null}]}, b_b: {$ifNull: ["$b", {$const: null}]}},
                sortBy: {time: 1, g: 1}
            }
        },
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        t_a: "$ts_0.t_a",
        t_b: "$ts_0.t_b",
        t_c: "$ts_0.t_c",
        b_a: "$bns_3.b_a",
        b_b: "$bns_3.b_b"
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
        t_b: {$top: {output: "$b", sortBy: {time: 1, g: 1}}},
        $willBeMerged: false
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
        b_b: {$bottom: {output: "$b", sortBy: {time: 1}}},
        $willBeMerged: false
    }
}
    )");
    verify(groupWithDifferentAccumulatorsWithSameSortPattern,
           makeVector(expectedNotOptimizedGroup));
}

TEST_F(CommonSortKeyOptimizationTest, OneTopAndOneTopNWithSameSortPatternNotOptimized) {
    // The different accumulator types _cannot_ be optimized though they have the same sort pattern.
    // If N were 1, then it could be optimized (see OneTopAndOneTopNWithSameSortPatternOptimized).
    const auto groupWithDifferentAccumulatorsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        t_a: {$top: {output: "$a", sortBy: {time: 1}}},
        t_b: {$topN: {n: 3, output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        t_a: {$top: {output: "$a", sortBy: {time: 1}}},
        t_b: {$topN: {n: {$const: 3}, output: "$b", sortBy: {time: 1}}},
        $willBeMerged: false
    }
}
    )");
    verify(groupWithDifferentAccumulatorsWithSameSortPattern,
           makeVector(expectedNotOptimizedGroup));
}

TEST_F(CommonSortKeyOptimizationTest, OneTopAndOneTopNWithSameSortPatternOptimized) {
    // The different accumulator types _can_ be optimized by first desugaring the $topN (N == 1) to
    // a $top, after which the two $top's can be grouped.
    const auto groupWithDifferentAccumulatorsWithSameSortPattern = fromjson(R"(
{
    $group: {
        _id: null,
        t_a: {$top: {output: "$a", sortBy: {time: 1}}},
        t_b: {$topN: {n: 1, output: "$b", sortBy: {time: 1}}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        ts_0: {$top: {
            output: {t_a: {$ifNull: ["$a", {$const: null}]}, t_b: {$ifNull: ["$b", {$const: null}]}},
            sortBy: {time: 1}
        }},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        t_a: "$ts_0.t_a",
        t_b: "$ts_0.t_b"
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {t_b: ["$t_b"]}
}
    )");
    verify(
        groupWithDifferentAccumulatorsWithSameSortPattern,
        makeVector(expectedOptimizedGroup, expectedOptimizedProject, expectedOptimizedAddFields));
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
        b_b: {$bottomN: {n: {$const: 3}, output: "$b", sortBy: {time: 1}}},
        $willBeMerged: false
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
        tns_0: {
            $topN: {
                n: {$const: 5},
                output: {t_a: {$ifNull: ["$a", {$const: null}]}, t_b: {$ifNull: ["$b", {$const: null}]}},
                sortBy: {time: 1, g: -1}
            }
        },
        fc: {$first: "$c"},
        ld: {$last: "$d"},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        t_a: "$tns_0.t_a",
        t_b: "$tns_0.t_b",
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
        tns_1: {
            $topN: {
                n: {$const: 5},
                output: {t_a: {$ifNull: ["$a", {$const: null}]}, t_b: {$ifNull: ["$b", {$const: null}]}},
                sortBy: {time: 1, g: -1}
            }
        },
        fc: {$first: "$c"},
        ld: {$last: "$d"},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        b_k: true,
        t_a: "$tns_1.t_a",
        t_b: "$tns_1.t_b",
        fc: true,
        ld: true
    }
}
    )");
    verify(groupWithOptimizableTopNsMixedWithFirstLastOneBottom,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

TEST_F(CommonSortKeyOptimizationTest,
       OptimizableTopNsBottomsWithDifferentNsAndSortOrdersAccumulators) {
    const auto groupWithTopNsBottomsWithDifferentNsAndSortOrders = fromjson(R"(
{
    $group: {
        _id: null,
        b_a_a: {$bottom: {output: "$a", sortBy: {a: 1}}},
        b_ab_b: {$bottom: {output: "$b", sortBy: {a: 1, b: 1}}},
        b_a_b: {$bottom: {output: "$b", sortBy: {a: 1}}},
        t3_a_b: {$topN: {n: 3, output: "$b", sortBy: {a: 1}}},
        b_ab_c: {$bottom: {output: "$c", sortBy: {a: 1, b: 1}}},
        b_ab_d: {$bottom: {output: "$d", sortBy: {a: 1, b: 1}}},
        t3_a_c: {$topN: {n: 3, output: "$c", sortBy: {a: 1}}},
        t5_a_c: {$topN: {n: 5, output: "$c", sortBy: {a: 1}}},
        t3_a_d: {$topN: {n: 3, output: "$d", sortBy: {a: 1}}},
        t5_a_d: {$topN: {n: 5, output: "$d", sortBy: {a: 1}}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        bs_0: {
            $bottom: {
                output: {
                    b_a_a: {$ifNull: ["$a", {$const: null}]},
                    b_a_b: {$ifNull: ["$b", {$const: null}]}
                },
                sortBy: {a: 1}
            }
        },
        bs_1: {
            $bottom: {
                output: {
                    b_ab_b: {$ifNull: ["$b", {$const: null}]},
                    b_ab_c: {$ifNull: ["$c", {$const: null}]},
                    b_ab_d: {$ifNull: ["$d", {$const: null}]}
                },
                sortBy: {a: 1, b: 1}
            }
        },
        tns_3: {
            $topN: {
                n: { $const: 3 },
                output: {
                    t3_a_b: {$ifNull: ["$b", {$const: null}]},
                    t3_a_c: {$ifNull: ["$c", {$const: null}]},
                    t3_a_d: {$ifNull: ["$d", {$const: null}]}
                },
                sortBy: {a: 1}
            }
        },
        tns_7: {
            $topN: {
                n: { $const: 5 },
                output: {
                    t5_a_c: {$ifNull: ["$c", {$const: null}]},
                    t5_a_d: {$ifNull: ["$d", {$const: null}]}
                },
                sortBy: {a: 1}
            }
        },
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        b_a_a: "$bs_0.b_a_a",
        b_ab_b: "$bs_1.b_ab_b",
        b_a_b: "$bs_0.b_a_b",
        t3_a_b: "$tns_3.t3_a_b",
        b_ab_c: "$bs_1.b_ab_c",
        b_ab_d: "$bs_1.b_ab_d",
        t3_a_c: "$tns_3.t3_a_c",
        t5_a_c: "$tns_7.t5_a_c",
        t3_a_d: "$tns_3.t3_a_d",
        t5_a_d: "$tns_7.t5_a_d"
    }
}
    )");
    verify(groupWithTopNsBottomsWithDifferentNsAndSortOrders,
           makeVector(expectedOptimizedGroup, expectedOptimizedProject));
}

}  // namespace
}  // namespace mongo
