/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/index/column_key_generator.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/inner_pipeline_stage_impl.h"
#include "mongo/db/pipeline/inner_pipeline_stage_interface.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
const std::string kIndexName = "indexName";
const BSONObj kKeyPattern = BSON("$**"
                                 << "columnstore");

/**
 * A specialization of the QueryPlannerTest fixture which makes it easy to present the planner with
 * a view of the available column indexes.
 */
class QueryPlannerColumnarTest : public QueryPlannerTest {
protected:
    void setUp() final {
        QueryPlannerTest::setUp();

        // Treat all queries as SBE compatible for this test.
        QueryPlannerTest::setMarkQueriesSbeCompatible(true);

        // We're interested in testing plans that use a columnar index, so don't generate collection
        // scans.
        params.options &= ~QueryPlannerParams::INCLUDE_COLLSCAN;
    }

    void tearDown() final {
        internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(
            kInternalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScanDefault);
        internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(
            kInternalQueryMaxNumberOfFieldsToChooseFilteredColumnScanDefault);
    }

    void addColumnStoreIndexAndEnableFilterSplitting(bool genPerColFilter = true,
                                                     StringData indexName = kIndexName,
                                                     const IndexPathProjection* proj = nullptr,
                                                     BSONObj keyPattern = kKeyPattern,
                                                     MatchExpression* partialFilterExpr = nullptr,
                                                     CollatorInterface* collator = nullptr) {
        params.columnStoreIndexes.emplace_back(keyPattern,
                                               IndexType::INDEX_COLUMN,
                                               IndexDescriptor::kLatestIndexVersion,
                                               false /* sparse */,
                                               false /* unique */,
                                               IndexEntry::Identifier{indexName.toString()},
                                               partialFilterExpr,
                                               collator,
                                               proj ? proj : &_defaultPathProj);
        if (genPerColFilter) {
            params.options |= QueryPlannerParams::GENERATE_PER_COLUMN_FILTERS;
        }
    }

    std::vector<std::unique_ptr<InnerPipelineStageInterface>> makeInnerPipelineStages(
        const Pipeline& pipeline) {
        std::vector<std::unique_ptr<InnerPipelineStageInterface>> stages;
        for (auto&& source : pipeline.getSources()) {
            stages.emplace_back(std::make_unique<InnerPipelineStageImpl>(source));
        }
        return stages;
    }

    IndexPathProjection makeProjection(BSONObj columnstoreProjection,
                                       BSONObj keyPattern = kKeyPattern) {
        return column_keygen::ColumnKeyGenerator::createProjectionExecutor(keyPattern,
                                                                           columnstoreProjection);
    }

private:
    // SBE must be enabled in order to test columnar indexes.
    RAIIServerParameterControllerForTest _controllerSBE{"internalQueryFrameworkControl",
                                                        "trySbeEngine"};
    IndexPathProjection _defaultPathProj =
        column_keygen::ColumnKeyGenerator::createProjectionExecutor(kKeyPattern, BSONObj());
};

TEST_F(QueryPlannerColumnarTest, InclusionProjectionUsesColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSON("a" << BSON("$gt" << 3)), BSONObj(), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        column_scan: {
            filtersByPath: {a: {a: {$gt: 3}}},
            outputFields: ['a'],
            matchFields: ['a']
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ComputedProjectionUsesColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(
        BSON("a" << BSON("$gt" << 3)),
        BSONObj(),
        BSON("a" << 1 << "foo" << BSON("$add" << BSON_ARRAY("$foo" << 1)) << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, foo: {$add: ["$foo", 1]}, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {a: {a: {$gt: 3}}},
                    outputFields: ['a', 'foo'],
                    matchFields: ['a']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ExpressionProjectionUsesColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSON("a" << BSON("$gt" << 3)), BSONObj(), fromjson(R"({
                         a: 1,
                         scaledA: {$multiply: ["$a", "$multiplier"]},
                         extra: {$literal: 4},
                         _id: 0
                     })"));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, scaledA: {$multiply: ["$a", "$multiplier"]}, extra: {$const: 4}, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {a: {a: {$gt: 3}}},
                    outputFields: ['a', 'multiplier'],
                    matchFields: ['a']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ImplicitlyIncludedIdIsIncludedInProjectedFields) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSON("a" << BSON("$gt" << 3)), BSONObj(), BSON("a" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        column_scan: {
            filtersByPath: {a: {a: {$gt: 3}}},
            outputFields: ['a', '_id'],
            matchFields: ['a']
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, InclusionProjectionWithSortUsesColumnStoreIndexAndBlockingSort) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSONObj(), BSON("a" << 1), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        sort: {
            pattern: {a: 1},
            limit: 0,
            node: {column_scan: {outputFields: ['a'], matchFields: []}}
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, SortOnSeparateColumnAddsThatColumnToColumnScan) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSONObj(), BSON("b" << 1), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                sort: {
                    pattern: {b: 1},
                    limit: 0,
                    node: {column_scan: {outputFields: ['a', 'b'], matchFields: []}}
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ExclusionProjectionDoesNotUseColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 0 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 0, _id: 0}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, NoProjectionDoesNotUseColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSON("a" << 1), BSONObj(), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(R"({cscan: {dir: 1, filter: {a: {$eq: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, ProjectionWithTooManyFieldsDoesNotUseColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "b" << 1 << "c" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, c: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, ExpressionProjectionWithTooManyFieldsDoesnotUseColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    // This will need 3 fields for the $concat, so should not be able to use a column scan.
    runQuerySortProj(BSONObj(), BSONObj(), fromjson("{str: {$concat: ['$a', '$b', '$c']}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        R"({proj: {spec: {str: {$concat: ['$a', '$b', '$c']}}, node: {cscan: {dir: 1}}}})");
}

// Test with a number of fields equal to the limit.
TEST_F(QueryPlannerColumnarTest, ImplicitIdCountsTowardsFieldLimit) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(
        BSONObj(), BSONObj(), BSON("a" << 1 << "b" << 1 /* _id implicitly included */));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, ProjectionWithJustEnoughFieldsDoesUseColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    // Without the '_id' this should be eligible.
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "b" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists("{column_scan: {outputFields: ['a', 'b']}}");
}

TEST_F(QueryPlannerColumnarTest, DottedProjectionTooManyFieldsDoesNotUseColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "b" << BSON("c" << 1 << "d" << 1)));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, 'b.c': 1, 'b.d': 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest,
       ProjectionWithTooManyFieldsDoesNotUseColumnStoreIndexUnsupportedPredicate) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSON("unsupported" << BSON("$exists" << false)),
                     BSONObj(),
                     BSON("a" << 1 << "b" << 1 << "c" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, c: 1}, node: {cscan: {dir: 1}}}})");
}

// Tests that a query which depends on overlapping parent/child fields like 'a.b' and 'a' will not
// use the column store index.
TEST_F(QueryPlannerColumnarTest, QueryWithOverlappingDependenciesDoesNotUseColumnarIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSONObj(), BSON("a.b" << 1 << "a.c" << 1), BSON("a" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        sort: {
            pattern: {"a.b": 1, "a.c": 1},
            limit: 0,
            node: {
                proj: {
                    spec: {a: 1},
                    node: {
                        cscan: {dir: 1}
                    }
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, QueryWithConflictingAncestralDependenciesDoesNotUseColumnarIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSONObj(), BSON("a.b.c" << 1), BSON("a" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        sort: {
            pattern: {"a.b.c": 1},
            limit: 0,
            node: {
                proj: {
                    spec: {a: 1},
                    node: {
                        cscan: {dir: 1}
                    }
                }
            }
        }
    })");
}

// Test like those above, but proving that we do the prefix detection correctly and don't mistake
// regular (non-path) prefixes.
TEST_F(QueryPlannerColumnarTest, QueryWithSimilarDependenciesDoesUseColumnarIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSONObj(), BSON("abc" << 1), BSON("a" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 1},
            node: {
                sort: {
                    pattern: {"abc": 1},
                    limit: 0,
                    node: {
                        column_scan: {
                            filtersByPath: {},
                            outputFields: ['_id', 'a', 'abc'],
                            matchFields: []
                        }
                    }
                }
            }
        }
    })");
}

// Test that adding a hint will allow you to use the column store index for a query with overlapping
// parent/child dependencies.
TEST_F(QueryPlannerColumnarTest, HintOverridesOverlappingFieldsCheck) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProjSkipLimitHint(BSONObj(),
                                  BSON("a.b.c" << 1),
                                  BSON("a" << 1),
                                  0,
                                  0,
                                  BSON("$**"
                                       << "columnstore"));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        sort: {
            pattern: {"a.b.c": 1},
            limit: 0,
            node: {
                proj: {
                    spec: {a: 1, _id: 1},
                    node: {
                        column_scan: {
                            filtersByPath: {},
                            outputFields: ['_id', 'a', 'a.b.c'],
                            matchFields: []
                        }
                    }
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, HintOverridesFieldLimitUnfiltered) {
    addColumnStoreIndexAndEnableFilterSplitting();
    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProjSkipLimitHint(BSONObj(),
                                  BSONObj(),
                                  BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1),
                                  0,
                                  0,
                                  BSON("$**"
                                       << "columnstore"));
    assertNumSolutions(1U);
    assertSolutionExists(R"({column_scan: {outputFields: ["a", "b", "c", "d", "_id"]}})");
}

TEST_F(QueryPlannerColumnarTest, HintOverridesFieldLimitFiltered) {
    addColumnStoreIndexAndEnableFilterSplitting();
    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(2);
    runQuerySortProjSkipLimitHint(BSON("a"
                                       << "selective"),
                                  BSONObj(),
                                  BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1),
                                  0,
                                  0,
                                  BSON("$**"
                                       << "columnstore"));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        column_scan: {
            outputFields: ["a", "b", "c", "d", "_id"],
            matchFields: ["a"]
        }
    })");
}

// TODO SERVER-66284 Ideally this wouldn't fail.
TEST_F(QueryPlannerColumnarTest, HintFailsWhenFetchIsRequired) {
    addColumnStoreIndexAndEnableFilterSplitting();
    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(2);
    ASSERT_THROWS(
        runQuerySortProjSkipLimitHint(BSON("a"
                                           << "selective"),
                                      BSONObj(),
                                      BSONObj(),
                                      0,
                                      0,
                                      BSON("$**"
                                           << "columnstore")),
        unittest::TestAssertionFailureException);  // Expect the test fixture to assert on status.
}

TEST_F(QueryPlannerColumnarTest, HintCausesErrorIfIndexNotFound) {
    // Missing addColumnStoreIndexAndEnableFilterSplitting();
    ASSERT_THROWS(
        runQuerySortProjSkipLimitHint(BSON("a"
                                           << "selective"),
                                      BSONObj(),
                                      BSON("a" << 1),
                                      0,
                                      0,
                                      BSON("$**"
                                           << "columnstore")),
        unittest::TestAssertionFailureException);  // Expect the test fixture to assert on status.
}

TEST_F(QueryPlannerColumnarTest, HintCausesErrorIfQueryDoesNotSupportSBE) {
    addColumnStoreIndexAndEnableFilterSplitting();
    setMarkQueriesSbeCompatible(false);
    ASSERT_THROWS(
        runQuerySortProjSkipLimitHint(BSON("a"
                                           << "selective"),
                                      BSONObj(),
                                      BSONObj(),
                                      0,
                                      0,
                                      BSON("$**"
                                           << "columnstore")),
        unittest::TestAssertionFailureException);  // Expect the test fixture to assert on status.
}

TEST_F(QueryPlannerColumnarTest, StandardIndexPreferredOverColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();
    addIndex(BSON("a" << 1));

    runQuerySortProj(BSON("a" << 5), BSONObj(), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, _id: 0}, node: {ixscan: {pattern: {a: 1}}}}})");
}

TEST_F(QueryPlannerColumnarTest, IneligiblePredicateNeedsToBeAppliedAfterAssembly) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSON("a" << BSONNULL), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        column_scan: {
            filtersByPath: {},
            outputFields: ['a'],
            matchFields: ['a'],
            postAssemblyFilter: {a: {$eq: null}}
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, MultiplePredicatesAllowedWithColumnStoreIndex) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSON("a" << 2 << "b" << 3), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {a: {a: {$eq: 2}}, b: {b: {$eq: 3}}},
                    outputFields: ['a'],
                    matchFields: ['a', 'b']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest,
       TooManyProjectedFieldsDisqualifiesColumnScanEvenWithEligiblePredicates) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(2);
    runQuerySortProj(BSON("a" << 2 << "b" << 3), BSONObj(), BSON("a" << 1 << "b" << 1 << "c" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, c: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, TooManyFilteredFieldsDisqualifiesColumnScan) {
    addColumnStoreIndexAndEnableFilterSplitting();

    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(2);
    runQuerySortProj(BSON("a" << 2 << "b" << 3 << "c" << 4),
                     BSONObj(),
                     BSON("a" << 1 << "b" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, _id: 0}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, FilterDependingOnWholeDocumentDisqualifiesColumnScan) {
    addColumnStoreIndexAndEnableFilterSplitting();

    // The projection only needs 1 field, but the match references '$$ROOT' so needs the whole
    // document.
    runQuerySortProj(
        BSON("$expr" << BSON("$eq" << BSON_ARRAY("$$ROOT" << BSON("b" << 2 << "c" << 3)))),
        BSONObj(),
        BSON("b" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {b: 1, _id: 0}, node: {cscan: {dir: 1}}}})");
}
TEST_F(QueryPlannerColumnarTest, CombinationOfProjectedAndMatchedFieldsDisqualifiesColumnScan) {
    addColumnStoreIndexAndEnableFilterSplitting();

    // Neither the match nor the project mentions 4 fields, but together they exceed the threshhold.
    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(4);
    runQuerySortProj(BSON("a" << 2 << "b" << 3 << "c" << 4),
                     BSONObj(),
                     BSON("c" << 1 << "d" << 1 << "e" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {c: 1, d: 1, e: 1, _id: 0}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, NumberOfFieldsComputedUsingSetSize) {
    addColumnStoreIndexAndEnableFilterSplitting();

    // If there are 3 fields referenced in the match and 3 in the projection, but they overlap, we
    // should be OK to use column scan.
    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(3);
    runQuerySortProj(BSON("a" << 2 << "b" << 3 << "c" << 4),
                     BSONObj(),
                     BSON("a" << 1 << "b" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, b: 1, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {a: {a: {$eq: 2}}, b: {b: {$eq: 3}}, c: {c: {$eq: 4}}},
                    outputFields: ['a', 'b'],
                    matchFields: ['a', 'b', 'c']
                }
            }
        }
    })");
}
TEST_F(QueryPlannerColumnarTest, ComplexPredicateSplitDemo) {
    addColumnStoreIndexAndEnableFilterSplitting();

    auto complexPredicate = fromjson(R"({
        a: {$gte: 0},
        "addresses.zip": "12345",
        unsubscribed: false
    })");
    runQuerySortProj(complexPredicate, BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {
                        a: {a: {$gte: 0}},
                        'addresses.zip': {'addresses.zip': {$eq: '12345'}},
                        unsubscribed: {unsubscribed: {$eq: false}}
                    },
                    outputFields: ['a'],
                    matchFields: ['a', 'addresses.zip', 'unsubscribed']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ComplexPredicateSplitsIntoParts) {
    addColumnStoreIndexAndEnableFilterSplitting();

    // Same predicate as above, except with exists: false, which disqualifies the whole thing.
    auto complexPredicate = fromjson(R"({
        a: {$gte: 0},
        "addresses.zip": "12345",
        unsubscribed: false,
        specialAddress: {$exists: false},
        doNotContact: {$exists: false}
    })");
    runQuerySortProj(complexPredicate, BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {
                        a: {a: {$gte: 0}},
                        'addresses.zip': {'addresses.zip': {$eq: '12345'}},
                        unsubscribed: {unsubscribed: false}
                    },
                    outputFields: ['a'],
                    postAssemblyFilter: {
                        specialAddress: {$exists: false},
                        doNotContact: {$exists: false}
                    },
                    matchFields:
                        ['a', 'addresses.zip', 'unsubscribed', 'specialAddress', 'doNotContact']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, EmptyQueryPredicateIsEligible) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(
        "{column_scan: {filtersByPath: {}, outputFields: ['a'], matchFields: []}}");
}

TEST_F(QueryPlannerColumnarTest, GroupTest) {
    addColumnStoreIndexAndEnableFilterSplitting();

    auto pipeline = Pipeline::parse({fromjson("{$group: {_id: '$foo', s: {$sum: '$x'}}}")}, expCtx);

    runQueryWithPipeline(
        BSONObj(), BSON("foo" << 1 << "x" << 1 << "_id" << 0), makeInnerPipelineStages(*pipeline));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        column_scan: {
            filtersByPath: {},
            outputFields: ['foo', 'x'],
            matchFields: []
        }
    })");

    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(R"({
        group: {
            key: {_id: '$foo'},
            accs: [{s: {$sum: '$x'}}],
            node: {
                column_scan: {
                    filtersByPath: {},
                    outputFields: ['foo', 'x'],
                    matchFields: []
                }
            }
        }
    })",
                                                   solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerColumnarTest, MatchGroupTest) {
    addColumnStoreIndexAndEnableFilterSplitting();

    auto pipeline = Pipeline::parse({fromjson("{$group: {_id: '$foo', s: {$sum: '$x'}}}")}, expCtx);

    runQueryWithPipeline(BSON("name"
                              << "bob"),
                         BSON("foo" << 1 << "x" << 1 << "_id" << 0),
                         makeInnerPipelineStages(*pipeline));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {foo: 1, x: 1, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {name: {name: {$eq: 'bob'}}},
                    outputFields: ['foo', 'x'],
                    matchFields: ['name']
                }
            }
        }
    })");

    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(R"({
        group: {
            key: {_id: '$foo'},
            accs: [{s: {$sum: '$x'}}],
            node: {
                column_scan: {
                    filtersByPath: {name: {name: {$eq: 'bob'}}},
                    outputFields: ['foo', 'x'],
                    matchFields: ['name']
                }
            }
        }
    })",
                                                   solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerColumnarTest, MatchGroupWithOverlappingFieldsTest) {
    addColumnStoreIndexAndEnableFilterSplitting();

    auto pipeline = Pipeline::parse(
        {fromjson("{$group: {_id: '$foo', s: {$sum: '$x'}, name: {$first: '$name'}}}")}, expCtx);

    runQueryWithPipeline(BSON("name"
                              << "bob"),
                         BSON("foo" << 1 << "x" << 1 << "name" << 1 << "_id" << 0),
                         makeInnerPipelineStages(*pipeline));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        column_scan: {
            filtersByPath: {name: {name: {$eq: 'bob'}}},
            outputFields: ['foo', 'x', 'name'],
            matchFields: ['name']
        }
    })");

    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(R"({
            group: {
                key: {_id: '$foo'},
                accs: [{s: {$sum: '$x'}}, {name: {$first: '$name'}}],
                node: {
                    column_scan: {
                        filtersByPath: {name: {name: {$eq: 'bob'}}},
                        outputFields: ['foo', 'x', 'name'],
                        matchFields: ['name']
                    }
                }
            }
        })",
                                                   solution->root()))
        << solution->root()->toString();
}

// Test that if a dotted path is requested then we need to add a PROJECTION_DEFAULT stage on top of
// the COLUMN_SCAN.
TEST_F(QueryPlannerColumnarTest, DottedFieldsRequireProjectionStage) {
    addColumnStoreIndexAndEnableFilterSplitting();

    runQuerySortProj(
        BSON("a" << BSON("$gt" << 3)), BSONObj(), BSON("a" << 1 << "b.c" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, 'b.c': 1, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {a: {a: {$gt: 3}}},
                    outputFields: ['a', 'b.c'],
                    matchFields: ['a']
                }
            }
        }
    })");
}

// As an exception to the above rule, a projection which is only including fields under a $group
// stage does not need the projection. The COLUMN_SCAN stage will output data in a format that is
// non-ambiguous for field path expressions like in a $group stage, but is not fully correct for a
// normal projection. This o
TEST_F(QueryPlannerColumnarTest, DottedFieldsWithGroupStageDoesNotRequireProjection) {
    addColumnStoreIndexAndEnableFilterSplitting();

    auto pipeline = Pipeline::parse(
        {fromjson("{$group: {_id: '$foo.bar', s: {$sum: '$x.y'}, name: {$first: '$name'}}}")},
        expCtx);

    runQueryWithPipeline(BSON("name"
                              << "bob"),
                         BSON("foo.bar" << 1 << "x.y" << 1 << "name" << 1 << "_id" << 0),
                         makeInnerPipelineStages(*pipeline));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {'foo.bar': 1, 'x.y': 1, name: 1, _id: 0},
            node: {
                column_scan: {
                    filtersByPath: {name: {name: {$eq: 'bob'}}},
                    outputFields: ['foo.bar', 'x.y', 'name'],
                    matchFields: ['name']
                }
            }
        }
    })");

    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(R"({
            group: {
                key: {_id: '$foo.bar'},
                accs: [{s: {$sum: '$x.y'}}, {name: {$first: '$name'}}],
                node: {
                    column_scan: {
                        filtersByPath: {name: {name: {$eq: 'bob'}}},
                        outputFields: ['foo.bar', 'x.y', 'name'],
                        matchFields: ['name']
                    }
                }
            }
        })",
                                                   solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerColumnarTest, ShardKeyFieldsIncluded) {
    addColumnStoreIndexAndEnableFilterSplitting();
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("sk1" << 1 << "sk2.nested" << 1);

    runQuerySortProj(BSON("name"
                          << "bob"),
                     BSONObj(),
                     BSON("foo" << 1 << "x" << 1 << "name" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {foo: 1, x: 1, name:1, _id: 0},
            node: {
                sharding_filter: {
                    node: {
                        column_scan: {
                            filtersByPath: {name: {name: {$eq: 'bob'}}},
                            outputFields: ['foo', 'x', 'name', 'sk1', 'sk2.nested'],
                            matchFields: ['name']
                        }
                    }
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ShardKeyFieldsCountTowardsFieldLimit) {
    addColumnStoreIndexAndEnableFilterSplitting();
    params.options |= QueryPlannerParams::INCLUDE_SHARD_FILTER;
    params.shardKey = BSON("sk1" << 1 << "sk2.nested" << 1);

    // Lower the upper bound on number of fields for COLUMN_SCAN eligibility. This should cause us
    // to choose a COLLSCAN instead of a COLUMN_SCAN.
    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(3);
    runQuerySortProj(BSON("name"
                          << "bob"),
                     BSONObj(),
                     BSON("foo" << 1 << "x" << 1 << "name" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {foo: 1, x: 1, name:1, _id: 0},
            node: {
                sharding_filter: {
                    node: {
                        cscan: {dir: 1}
                    }
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, SelectsFirstFromMultipleEligibleColumnStoreIndexes) {
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index"_sd);
    addColumnStoreIndexAndEnableFilterSplitting(false, "second index"_sd);

    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertSolutionExists(R"({
        column_scan: {
            indexName: 'first index',
            outputFields: ['a'],
            matchFields: []
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, UseColumnStoreWithExactFields) {
    auto firstProj = makeProjection(fromjson(R"({"d": true, "b.c": true, "_id": false})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index"_sd, &firstProj);

    auto secondProj = makeProjection(fromjson(R"({"a": true, "b.c": true, "_id": false})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "second index"_sd, &secondProj);

    // Should use the second index, despite the third index being valid, because the second index
    // was seen first.
    auto thirdProj = makeProjection(fromjson(R"({"a": true, "b.c": true, "_id": false})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "third index"_sd, &thirdProj);

    runQuerySortProj(
        BSON("a" << BSON("$gt" << 3)), BSONObj(), BSON("a" << 1 << "b.c" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, 'b.c': 1, _id: 0},
            node: {
                column_scan: {
                    indexName: 'second index',
                    filtersByPath: {a: {a: {$gt: 3}}},
                    outputFields: ['a', 'b.c'],
                    matchFields: ['a']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, UseColumnStoreWithExtraFields) {
    auto firstProj = makeProjection(fromjson(
        R"({"a": true, "unsubscribed": true, "test field": true, "another test field": true, "_id": false})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index", &firstProj);

    auto secondProj = makeProjection(fromjson(R"({
            "a": true,
            "addresses.zip": true,
            "unsubscribed": true,
            "specialAddress": true,
            "doNotContact": true,
            "test field": true,
            "another test field": true,
            "_id": false
    })"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "second index"_sd, &secondProj);

    // Same predicate as above, except with exists: false, which disqualifies the whole thing.
    auto complexPredicate = fromjson(R"({
        a: {$gte: 0},
        "addresses.zip": "12345",
        unsubscribed: false,
        specialAddress: {$exists: false},
        doNotContact: {$exists: false}
    })");
    runQuerySortProj(complexPredicate, BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_scan: {
                    index_name: 'second index',
                    filtersByPath: {
                        a: {a: {$gte: 0}},
                        'addresses.zip': {'addresses.zip': {$eq: '12345'}},
                        unsubscribed: {unsubscribed: false}
                    },
                    outputFields: ['a'],
                    postAssemblyFilter: {
                        specialAddress: {$exists: false},
                        doNotContact: {$exists: false}
                    },
                    matchFields:
                        ['a', 'addresses.zip', 'unsubscribed', 'specialAddress', 'doNotContact']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, UseColumnStoreWithSinglePath) {
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index"_sd);
    addColumnStoreIndexAndEnableFilterSplitting(true, "second index"_sd);


    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        column_scan: {
            indexName: 'first index',
            filtersByPath: {},
            outputFields: ['a'],
            matchFields: []
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, UseColumnStoreWithAncestorField) {
    auto firstProj = makeProjection(fromjson(R"({"foo": true, "x": true, "name": true})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index"_sd, &firstProj);

    auto secondProj = makeProjection(BSONObj(),
                                     BSON("foo.$**"
                                          << "columnstore"));
    addColumnStoreIndexAndEnableFilterSplitting(true,
                                                "second index"_sd,
                                                &secondProj,
                                                BSON("foo.$**"
                                                     << "columnstore"));

    auto pipeline = Pipeline::parse(
        {fromjson("{$group: {_id: '$foo.bar', s: {$sum: '$x.y'}, name: {$first: '$name'}}}")},
        expCtx);

    runQueryWithPipeline(BSON("name"
                              << "bob"),
                         BSON("foo.bar" << 1 << "x.y" << 1 << "name" << 1 << "_id" << 0),
                         makeInnerPipelineStages(*pipeline));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {'foo.bar': 1, 'x.y': 1, name: 1, _id: 0},
            node: {
                column_scan: {
                    indexName: 'first index',
                    filtersByPath: {name: {name: {$eq: 'bob'}}},
                    outputFields: ['foo.bar', 'x.y', 'name'],
                    matchFields: ['name']
                }
            }
        }
    })");

    ASSERT(!cq->pipeline().empty());
    auto solution =
        QueryPlanner::extendWithAggPipeline(*cq, std::move(solns[0]), {} /* secondaryCollInfos
        */);
    ASSERT_OK(QueryPlannerTestLib::solutionMatches(R"({
            group: {
                key: {_id: '$foo.bar'},
                accs: [{s: {$sum: '$x.y'}}, {name: {$first: '$name'}}],
                node: {
                    column_scan: {
                        indexName: 'first index',
                        filtersByPath: {name: {name: {$eq: 'bob'}}},
                        outputFields: ['foo.bar', 'x.y', 'name'],
                        matchFields: ['name']
                    }
                }
            }
        })",
                                                   solution->root()))
        << solution->root()->toString();
}

TEST_F(QueryPlannerColumnarTest, DontUseColumnStoreWithSinglePath) {
    auto firstProj = makeProjection(BSONObj(),
                                    BSON("a.$**"
                                         << "columnstore"));
    addColumnStoreIndexAndEnableFilterSplitting(true,
                                                "first index"_sd,
                                                &firstProj,
                                                BSON("a.$**"
                                                     << "columnstore"));
    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSONObj(), BSONObj(), BSON("b" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {b: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, DontUseColumnStoreMissingField) {
    auto firstProj = makeProjection(fromjson(R"({"a": false})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index"_sd, &firstProj);

    auto secondProj = makeProjection(fromjson(R"({"b": true})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "second index"_sd, &secondProj);

    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, _id: 0}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, DontUseColumnStoreMissingMultipleField) {
    auto firstProj = makeProjection(fromjson(R"({"a": true, "c": true, "d": true})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index"_sd, &firstProj);

    auto secondProj = makeProjection(fromjson(R"({"b": true, "c": true, "d": true})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "second index"_sd, &secondProj);

    runQuerySortProj(BSON("a" << 1), BSONObj(), BSON("a" << true << "b" << true));
    assertNumSolutions(1U);
    assertSolutionExists(
        R"({proj: {spec: {a: 1, b: 1}, node: {cscan: {dir: 1, filter: {a: {$eq: 1}}}}}})");
}

TEST_F(QueryPlannerColumnarTest, DontUseColumnStoreSpecifiedSubField) {
    auto firstProj = makeProjection(fromjson(R"({"a.b": true, "b.c": true, "c": true})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first_index"_sd, &firstProj);

    auto secondProj = makeProjection(fromjson(R"({"a": true, "b.c": true, "d": true})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "second_index"_sd, &secondProj);

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "c" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, c: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, HintIndexDoesNotCoverQuery) {
    // Column Store Index does not cover query.
    auto firstProj = makeProjection(BSONObj(),
                                    BSON("b.$**"
                                         << "columnstore"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index", &firstProj);
    ASSERT_THROWS(runQuerySortProjSkipLimitHint(BSONObj(),
                                                BSONObj(),
                                                BSON("a" << 1 << "_id" << 0),
                                                0,
                                                0,
                                                BSON("$hint"
                                                     << "first index")),
                  unittest::TestAssertionFailureException);
}

TEST_F(QueryPlannerColumnarTest, NoColumnIndexCoversQuery) {
    auto firstProj = makeProjection(fromjson(R"({b: 1, d: 1})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "first index", &firstProj);
    auto secondProj = makeProjection(fromjson(R"({c: 1, d: 1})"));
    addColumnStoreIndexAndEnableFilterSplitting(true, "second index", &secondProj);

    // Valid for column scan, but no column store indices that cover the query.
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1}, node: {cscan: {dir: 1}}}})");
}

}  // namespace mongo
