// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for mongo/db/query/plan_ranker.h
 */

#include "mongo/db/query/plan_ranker.h"
#include "mongo/unittest/unittest.h"

namespace mongo::plan_ranker {
namespace {
const auto testNss = NamespaceString::createNamespaceString_forTest("testdb.coll");

IndexEntry buildSimpleIndexEntry(const BSONObj& kp) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier("test_foo"),
            {},
            nullptr};
}

OrderedIntervalList makeOIL(const std::string& fieldName,
                            std::initializer_list<BSONObj> intervals) {
    OrderedIntervalList oil(fieldName);
    for (auto&& interval : intervals) {
        oil.intervals.emplace_back(std::move(interval), true, true);
    }
    return oil;
}

std::unique_ptr<QuerySolution> makeSolution(std::unique_ptr<QuerySolutionNode> child) {
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::make_unique<FetchNode>(std::move(child), testNss));
    return solution;
}

std::unique_ptr<IndexScanNode> makeIndexScan(
    BSONObj indexKey, std::initializer_list<OrderedIntervalList> indexBoundsFields) {
    IndexBounds bounds{};
    for (auto&& field : indexBoundsFields) {
        bounds.fields.emplace_back(std::move(field));
    }

    auto indexScan = std::make_unique<IndexScanNode>(testNss, buildSimpleIndexEntry(indexKey));
    indexScan->bounds = std::move(bounds);
    indexScan->computeProperties();

    return indexScan;
}
}  // namespace

TEST(IndexPrefixHeuristicTest, SingleIndexScan_LongerSinglePointPrefixWins) {
    // a: [[1, 1]]; b: [[1, 3]]
    auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                    {
                                        makeOIL("a", {BSON("" << 1 << "" << 1)}),
                                        makeOIL("b", {BSON("" << 1 << "" << 3)}),
                                    });
    auto solution1 = makeSolution(std::move(indexScan1));

    // c: [[1, 1]]; d: [[1, 1]]
    auto indexScan2 = makeIndexScan(BSON("c" << 1 << "d" << 1),
                                    {
                                        makeOIL("c", {BSON("" << 1 << "" << 1)}),
                                        makeOIL("d", {BSON("" << 1 << "" << 1)}),
                                    });
    auto solution2 = makeSolution(std::move(indexScan2));

    // e: [[1, 1]]; f: [[2, 2], [3, 3]]; g: [[4, 4]]
    auto indexScan3 =
        makeIndexScan(BSON("e" << 1 << "f" << 1 << "g" << 1),
                      {
                          makeOIL("e", {BSON("" << 1 << "" << 1)}),
                          makeOIL("f", {BSON("" << 2 << "" << 2), BSON("" << 3 << "" << 3)}),
                          makeOIL("g", {BSON("" << 4 << "" << 4)}),
                      });
    auto solution3 = makeSolution(std::move(indexScan3));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get(), solution3.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(1, winners.size());
    ASSERT_EQ(1, winners[0]);
}

TEST(IndexPrefixHeuristicTest, SingleIndexScan_LongerPointPrefixWins) {
    // a: [[1, 1], [2, 2]]; b: [[1, 3]]
    auto indexScan1 =
        makeIndexScan(BSON("a" << 1 << "b" << 1),
                      {
                          makeOIL("a", {BSON("" << 1 << "" << 1), BSON("" << 2 << "" << 2)}),
                          makeOIL("b", {BSON("" << 1 << "" << 3)}),
                      });
    auto solution1 = makeSolution(std::move(indexScan1));

    // c: [[1, 1], [2, 2]]; d: [[1, 1], [3, 3]], e: [[1, 1]]
    auto indexScan2 =
        makeIndexScan(BSON("c" << 1 << "d" << 1 << "e" << 1),
                      {
                          makeOIL("c", {BSON("" << 1 << "" << 1), BSON("" << 2 << "" << 2)}),
                          makeOIL("d", {BSON("" << 1 << "" << 1), BSON("" << 3 << "" << 3)}),
                          makeOIL("e", {BSON("" << 1 << "" << 1)}),
                      });
    auto solution2 = makeSolution(std::move(indexScan2));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(1, winners.size());
    ASSERT_EQ(1, winners[0]);
}

TEST(IndexPrefixHeuristicTest, SingleIndexScan_ClosedIntervalPrefixWins) {
    // a: [[1, 1], [2, 2]]; b: [[1, MaxKey]]
    auto indexScan1 =
        makeIndexScan(BSON("a" << 1 << "b" << 1),
                      {
                          makeOIL("a", {BSON("" << 1 << "" << 1), BSON("" << 2 << "" << 2)}),
                          makeOIL("b", {BSON("" << 1 << "" << MAXKEY)}),
                      });
    auto solution1 = makeSolution(std::move(indexScan1));

    // c: [[1, 1], [2, 2]]; d: [[1, 3]]
    auto indexScan2 =
        makeIndexScan(BSON("c" << 1 << "d" << 1 << "e" << 1),
                      {
                          makeOIL("c", {BSON("" << 1 << "" << 1), BSON("" << 2 << "" << 2)}),
                          makeOIL("b", {BSON("" << 1 << "" << 3)}),
                      });
    auto solution2 = makeSolution(std::move(indexScan2));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(1, winners.size());
    ASSERT_EQ(1, winners[0]);
}

TEST(IndexPrefixHeuristicTest, SingleIndexScan_LongerPrefixWins) {
    // a: [[2, 3]]; b: [[1, 1]]
    auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                    {
                                        makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                        makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                    });
    auto solution1 = makeSolution(std::move(indexScan1));

    // c: [[5, 10]]; d: [[10, 20]], e: [[1, 101]]
    auto indexScan2 = makeIndexScan(BSON("c" << 1 << "d" << 1 << "e" << 1),
                                    {
                                        makeOIL("c", {BSON("" << 5 << "" << 10)}),
                                        makeOIL("d", {BSON("" << 10 << "" << 20)}),
                                        makeOIL("e", {BSON("" << 1 << "" << 101)}),
                                    });
    auto solution2 = makeSolution(std::move(indexScan2));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(1, winners.size());
    ASSERT_EQ(1, winners[0]);
}

TEST(IndexPrefixHeuristicTest, SingleIndexScan_MultipleWinners) {
    // a: [[2, 2]]; b: [[1, 1]]
    auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                    {
                                        makeOIL("a", {BSON("" << 2 << "" << 2)}),
                                        makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                    });
    auto solution1 = makeSolution(std::move(indexScan1));

    // c: [[5, 5]]; d: [[10, 20]]
    auto indexScan2 = makeIndexScan(BSON("c" << 1 << "d" << 1),
                                    {
                                        makeOIL("c", {BSON("" << 5 << "" << 5)}),
                                        makeOIL("d", {BSON("" << 10 << "" << 10)}),
                                    });
    auto solution2 = makeSolution(std::move(indexScan2));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(2, winners.size());
}

TEST(IndexPrefixHeuristicTest, SingleIndexScan_ShortestIndexKeyWins) {
    // a: [[2, 2]]; b: [[MinKey, MaxKey]]
    auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                    {
                                        makeOIL("a", {BSON("" << 2 << "" << 2)}),
                                        makeOIL("b", {BSON("" << MINKEY << "" << MAXKEY)}),
                                    });
    auto solution1 = makeSolution(std::move(indexScan1));

    // c: [[5, 5]]
    auto indexScan2 = makeIndexScan(BSON("c" << 1),
                                    {
                                        makeOIL("c", {BSON("" << 5 << "" << 5)}),
                                    });
    auto solution2 = makeSolution(std::move(indexScan2));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(1, winners.size());
    ASSERT_EQ(1, winners[0]);
}

TEST(IndexPrefixHeuristicTest, SingleIndexScan_DifferentQueryShapesNoWin) {
    // FETCH <- INDEX_SCAN
    auto indexScan =
        makeIndexScan(BSON("a" << 1 << "b" << 1),
                      {
                          makeOIL("a", {BSON("" << 1 << "" << 1), BSON("" << 2 << "" << 2)}),
                          makeOIL("b", {BSON("" << 1 << "" << 3)}),
                      });
    auto solution1 = makeSolution(std::move(indexScan));

    // FETCH <- COLLECTION_SCAN
    auto collScan = std::make_unique<CollectionScanNode>();
    auto solution2 = makeSolution(std::move(collScan));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(0, winners.size());
}


TEST(IndexPrefixHeuristicTest, UnionIndexScan_ShortestIndexKeyWins) {
    auto orNode1 = std::make_unique<OrNode>();
    {
        // a: [[2, 2]]; b: [[1, 1]]
        auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                        {
                                            makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                            makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                        });

        // c: [[2, 2]], d: [[MaxKey, MinKey]]
        auto indexScan2 = makeIndexScan(BSON("c" << 1 << "d" << 1),
                                        {
                                            makeOIL("c", {BSON("" << 2 << "" << 2)}),
                                            makeOIL("d", {BSON("" << MAXKEY << "" << MINKEY)}),
                                        });
        orNode1->children.emplace_back(std::move(indexScan1));
        orNode1->children.emplace_back(std::move(indexScan2));
    }
    auto solution1 = makeSolution(std::move(orNode1));

    auto orNode2 = std::make_unique<OrNode>();
    {
        // a: [[2, 2]]; b: [[1, 1]]
        auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                        {
                                            makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                            makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                        });

        // c: [[2, 2]]
        auto indexScan2 = makeIndexScan(BSON("c" << 1),
                                        {
                                            makeOIL("c", {BSON("" << 2 << "" << 2)}),
                                        });
        orNode2->children.emplace_back(std::move(indexScan1));
        orNode2->children.emplace_back(std::move(indexScan2));
    }
    auto solution2 = makeSolution(std::move(orNode2));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(1, winners.size());
    ASSERT_EQ(1, winners[0]);
}

TEST(IndexPrefixHeuristicTest, UnionIndexScan_MultipleWinners) {
    auto orNode1 = std::make_unique<OrNode>();
    {
        // a: [[2, 2]]; b: [[1, 1]]
        auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                        {
                                            makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                            makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                        });

        // c: [[2, 2]]
        auto indexScan2 = makeIndexScan(BSON("c" << 1),
                                        {
                                            makeOIL("c", {BSON("" << 2 << "" << 2)}),
                                        });
        orNode1->children.emplace_back(std::move(indexScan1));
        orNode1->children.emplace_back(std::move(indexScan2));
    }
    auto solution1 = makeSolution(std::move(orNode1));

    auto orNode2 = std::make_unique<OrNode>();
    {
        // a: [[2, 2]]; b: [[1, 1]]
        auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                        {
                                            makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                            makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                        });

        // c: [[2, 2]], d: [[MaxKey, MinKey]]
        auto indexScan2 = makeIndexScan(BSON("c" << 1 << "d" << 1),
                                        {
                                            makeOIL("c", {BSON("" << 2 << "" << 2)}),
                                            makeOIL("d", {BSON("" << MAXKEY << "" << MINKEY)}),
                                        });
        orNode2->children.emplace_back(std::move(indexScan1));
        orNode2->children.emplace_back(std::move(indexScan2));
    }
    auto solution2 = makeSolution(std::move(orNode2));

    auto orNode3 = std::make_unique<OrNode>();
    {
        // a: [[2, 2]]; b: [[1, 1]]
        auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                        {
                                            makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                            makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                        });

        // c: [[2, 2]]
        auto indexScan2 = makeIndexScan(BSON("c" << 1),
                                        {
                                            makeOIL("c", {BSON("" << 2 << "" << 2)}),
                                        });
        orNode3->children.emplace_back(std::move(indexScan1));
        orNode3->children.emplace_back(std::move(indexScan2));
    }
    auto solution3 = makeSolution(std::move(orNode3));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get(), solution3.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(2, winners.size());
    ASSERT_EQ(0, winners[0]);
    ASSERT_EQ(2, winners[1]);
}

TEST(IndexPrefixHeuristicTest, UnionIndexScan_DifferentQueryShapeNoWin) {
    auto orNode1 = std::make_unique<OrNode>();
    {
        // a: [[2, 2]]; b: [[1, 1]]
        auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                        {
                                            makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                            makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                        });

        // c: [[2, 2]], d: [[MaxKey, MinKey]]
        auto indexScan2 = makeIndexScan(BSON("c" << 1 << "d" << 1),
                                        {
                                            makeOIL("c", {BSON("" << 2 << "" << 2)}),
                                            makeOIL("d", {BSON("" << MAXKEY << "" << MINKEY)}),
                                        });
        orNode1->children.emplace_back(std::move(indexScan1));
        orNode1->children.emplace_back(std::move(indexScan2));
    }
    auto solution1 = makeSolution(std::move(orNode1));

    auto orNode2 = std::make_unique<OrNode>();
    {
        // a: [[2, 2]]; b: [[1, 1]]
        auto indexScan1 = makeIndexScan(BSON("a" << 1 << "b" << 1),
                                        {
                                            makeOIL("a", {BSON("" << 2 << "" << 3)}),
                                            makeOIL("b", {BSON("" << 1 << "" << 1)}),
                                        });

        // c: [[2, 2]], d: [[MaxKey, MinKey]]
        auto indexScan2 = makeIndexScan(BSON("c" << 1 << "d" << 1),
                                        {
                                            makeOIL("c", {BSON("" << 2 << "" << 2)}),
                                            makeOIL("d", {BSON("" << MAXKEY << "" << MINKEY)}),
                                        });

        // e: [[2, 2]]
        auto indexScan3 = makeIndexScan(BSON("e" << 1),
                                        {
                                            makeOIL("e", {BSON("" << 2 << "" << 2)}),
                                        });
        orNode2->children.emplace_back(std::move(indexScan1));
        orNode2->children.emplace_back(std::move(indexScan2));
        orNode2->children.emplace_back(std::move(indexScan3));
    }
    auto solution2 = makeSolution(std::move(orNode2));

    std::vector<const QuerySolution*> solutions{solution1.get(), solution2.get()};
    auto winners = applyIndexPrefixHeuristic(solutions);
    ASSERT_EQ(0, winners.size());
}
}  // namespace mongo::plan_ranker
