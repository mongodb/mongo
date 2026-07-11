// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace {

class AccNforNis1OptimizationTest : public AggregationContextFixture {
protected:
    void verify(const BSONObj& groupSpec, const std::vector<BSONObj>& expectedOptimizedPipeline) {
        auto pipeline = pipeline_factory::makePipeline(
            makeVector(groupSpec), getExpCtx(), pipeline_factory::kOptionsMinimal);

        ASSERT_EQ(pipeline->size(), 1U);

        pipeline_optimization::optimizePipeline(*pipeline);

        auto actualOptimizedPipeline = pipeline->serializeToBson();
        ASSERT_EQ(actualOptimizedPipeline.size(), expectedOptimizedPipeline.size())
            << "Expected " << expectedOptimizedPipeline.size() << " stages but got the following "
            << actualOptimizedPipeline.size() << " stages: " << to_string(actualOptimizedPipeline);

        for (size_t i = 0; i < actualOptimizedPipeline.size(); ++i) {
            ASSERT_BSONOBJ_EQ_UNORDERED(actualOptimizedPipeline[i], expectedOptimizedPipeline[i]);
        }
    }
};

TEST_F(AccNforNis1OptimizationTest, FirstNForNis1Optimized) {
    const auto groupWithFirstN = fromjson(R"(
{
    $group: {
        _id: null,
        field: {$firstN: {input: "$m", n: 1}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        field: {$first: "$m"},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {field: ["$field"]}
}
    )");

    verify(groupWithFirstN, makeVector(expectedOptimizedGroup, expectedOptimizedAddFields));
}

TEST_F(AccNforNis1OptimizationTest, LastNForNis1Optimized) {
    const auto groupWithLastN = fromjson(R"(
{
    $group: {
        _id: null,
        field: {$lastN: {input: "$m", n: 1}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        field: {$last: "$m"},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {field: ["$field"]}
}
    )");

    verify(groupWithLastN, makeVector(expectedOptimizedGroup, expectedOptimizedAddFields));
}

TEST_F(AccNforNis1OptimizationTest, TopNForNis1Optimized) {
    const auto groupWithTopN = fromjson(R"(
{
    $group: {
        _id: null,
        field: {$topN: {n: 1, sortBy: {time: 1}, output: "$m"}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        field: {$top: {sortBy: {time: 1}, output: "$m"}},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {field: ["$field"]}
}
    )");

    verify(groupWithTopN, makeVector(expectedOptimizedGroup, expectedOptimizedAddFields));
}

TEST_F(AccNforNis1OptimizationTest, BottomNForNis1Optimized) {
    const auto groupWithBottomN = fromjson(R"(
{
    $group: {
        _id: null,
        field: {$bottomN: {n: 1, sortBy: {time: 1}, output: "$m"}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        field: {$bottom: {sortBy: {time: 1}, output: "$m"}},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {field: ["$field"]}
}
    )");

    verify(groupWithBottomN, makeVector(expectedOptimizedGroup, expectedOptimizedAddFields));
}

TEST_F(AccNforNis1OptimizationTest, MultipleFirstNisOptimized) {
    const auto groupWithMultipleFirstN = fromjson(R"(
{
    $group: {
        _id: null,
        f1: {$firstN: {input: "$m", n: 1}},
        f2: {$firstN: {input: "$i", n: 1}},
        f3: {$firstN: {input: "$k", n: 1}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        f1: {$first: "$m"},
        f2: {$first: "$i"},
        f3: {$first: "$k"},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {f1: ["$f1"], f2: ["$f2"], f3: ["$f3"]}
}
    )");

    verify(groupWithMultipleFirstN, makeVector(expectedOptimizedGroup, expectedOptimizedAddFields));
}

TEST_F(AccNforNis1OptimizationTest, CompatibleLastAccsOptimized) {
    const auto groupWithBothLastAndLastN = fromjson(R"(
{
    $group: {
        _id: null,
        f1: {$last: "$m"},
        f2: {$lastN: {input: "$i", n: 1}}
    }
}
    )");
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        f1: {$last: "$m"},
        f2: {$last: "$i"},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {f2: ["$f2"]}
}
    )");

    verify(groupWithBothLastAndLastN,
           makeVector(expectedOptimizedGroup, expectedOptimizedAddFields));
}

TEST_F(AccNforNis1OptimizationTest, CompatibleBottomAccsOptimized) {
    const auto groupWithBothBottomAndBottomN = fromjson(R"(
{
    $group: {
        _id: null,
        f1: {$bottomN: {n: 1, sortBy: {time: 1}, output: "$a"}},
        f2: {$bottom: {sortBy: {time: 1}, output: "$b"}}
    }
}
    )");

    // The optimization to group common sort keys should be applied after the $bottomN has been
    // desugared.
    const auto expectedOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        bs_0: {$bottom: {
            sortBy: {time: 1},
            output: {f1: {$ifNull: ["$a", {$const: null}]}, f2: {$ifNull: ["$b", {$const: null}]}}
        }},
        $willBeMerged: false
    }
}
    )");
    const auto expectedOptimizedProject = fromjson(R"(
{
    $project: {
        _id: true,
        f1: "$bs_0.f1",
        f2: "$bs_0.f2"
    }
}
    )");
    const auto expectedOptimizedAddFields = fromjson(R"(
{
    $addFields: {f1: ["$f1"]}
}
    )");

    verify(
        groupWithBothBottomAndBottomN,
        makeVector(expectedOptimizedGroup, expectedOptimizedProject, expectedOptimizedAddFields));
}

TEST_F(AccNforNis1OptimizationTest, DifferentAccNnotOptimized) {
    const auto groupWithDifferentAccNs = fromjson(R"(
{
    $group: {
        _id: null,
        f1: {$firstN: {input: "$m", n: 1}},
        f2: {$lastN: {input: "$i", n: 1}}
    }
}
    )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        f1: {$firstN: {input: "$m", n: {$const: 1}}},
        f2: {$lastN: {input: "$i", n: {$const: 1}}},
        $willBeMerged: false
    }
}
    )");

    verify(groupWithDifferentAccNs, makeVector(expectedNotOptimizedGroup));
}

TEST_F(AccNforNis1OptimizationTest, AccNforNdifferentThan1NotOptimized) {
    const auto groupWithAccsWhereNisDifferentThan1 = fromjson(R"(
{
    $group: {
        _id: null,
        f1: {$lastN: {input: "$i", n: 1}},
        f2: {$lastN: {input: "$i", n: 5}}
    }
}
        )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        f1: {$lastN: {input: "$i", n: {$const: 1}}},
        f2: {$lastN: {input: "$i", n: {$const: 5}}},
        $willBeMerged: false
    }
}
        )");

    verify(groupWithAccsWhereNisDifferentThan1, makeVector(expectedNotOptimizedGroup));
}

TEST_F(AccNforNis1OptimizationTest, AccNwithDifferentSortKeyNotOptimized) {
    const auto groupWithAccsWithDifferentSortKey = fromjson(R"(
{
    $group: {
        _id: null,
        f1: {$topN: {n: 1, sortBy: {time: 1}, output: "$i"}},
        f2: {$topN: {n: 1, sortBy: {age: 1}, output: "$i"}}
    }
}
        )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        f1: {$topN: {n: {$const: 1}, sortBy: {time: 1}, output: "$i"}},
        f2: {$topN: {n: {$const: 1}, sortBy: {age: 1}, output: "$i"}},
        $willBeMerged: false
    }
}
        )");

    verify(groupWithAccsWithDifferentSortKey, makeVector(expectedNotOptimizedGroup));
}

TEST_F(AccNforNis1OptimizationTest, CompatibleAccsWithDifferentSortKeyNotOptimized) {
    const auto groupWithCompatibleAccButDifferentSortKey = fromjson(R"(
{
    $group: {
        _id: null,
        f1: {$topN: {n: 1, sortBy: {time: 1}, output: "$i"}},
        f2: {$top: {sortBy: {age: 1}, output: "$i"}}
    }
}
        )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        f1: {$topN: {n: {$const: 1}, sortBy: {time: 1}, output: "$i"}},
        f2: {$top: {sortBy: {age: 1}, output: "$i"}},
        $willBeMerged: false
    }
}
        )");

    verify(groupWithCompatibleAccButDifferentSortKey, makeVector(expectedNotOptimizedGroup));
}

TEST_F(AccNforNis1OptimizationTest, MinNnotOptimized) {
    const auto groupWithMinN = fromjson(R"(
{
    $group: {
        _id: null,
        field: {$minN: {input: "$m", n: 1}}
    }
}
        )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        field: {$minN: {input: "$m", n: {$const: 1}}},
        $willBeMerged: false
    }
}
        )");

    verify(groupWithMinN, makeVector(expectedNotOptimizedGroup));
}

TEST_F(AccNforNis1OptimizationTest, MaxNnotOptimized) {
    const auto groupWithMaxN = fromjson(R"(
{
    $group: {
        _id: null,
        field: {$maxN: {input: "$m", n: 1}}
    }
}
        )");
    const auto expectedNotOptimizedGroup = fromjson(R"(
{
    $group: {
        _id: {$const: null},
        field: {$maxN: {input: "$m", n: {$const: 1}}},
        $willBeMerged: false
    }
}
        )");

    verify(groupWithMaxN, makeVector(expectedNotOptimizedGroup));
}
}  // namespace
}  // namespace mongo
