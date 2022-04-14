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

#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
const std::string kIndexName = "indexName";

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

    void addColumnarIndex() {
        params.columnarIndexes.emplace_back(kIndexName);
    }
};

TEST_F(QueryPlannerColumnarTest, InclusionProjectionUsesColumnarIndex) {
    addColumnarIndex();

    runQuerySortProj(BSON("a" << BSON("$gt" << 3)), BSONObj(), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_ixscan:
                    {filtersByPath: {a: {a: {$gt: 3}}}, outputFields: ['a'], matchFields: ['a']}
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ExpressionProjectionUsesColumnarIndex) {
    addColumnarIndex();

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
                column_ixscan: {
                    filtersByPath: {a: {a: {$gt: 3}}},
                    outputFields: ['a', 'multiplier'],
                    matchFields: ['a']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ImplicitlyIncludedIdIsIncludedInProjectedFields) {
    addColumnarIndex();

    runQuerySortProj(BSON("a" << BSON("$gt" << 3)), BSONObj(), BSON("a" << 1));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1},
            node: {
                column_ixscan: {
                    filtersByPath: {a: {a: {$gt: 3}}},
                    outputFields: ['a', '_id'],
                    matchFields: ['a']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, InclusionProjectionWithSortUsesColumnarIndexAndBlockingSort) {
    addColumnarIndex();

    runQuerySortProj(BSONObj(), BSON("a" << 1), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        sort: {
            pattern: {a: 1},
            limit: 0,
            node: {
                proj: {
                    spec: {a: 1, _id: 0},
                    node: {column_ixscan: {outputFields: ['a'], matchFields: []}}
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, SortOnSeparateColumnAddsThatColumnToColumnScan) {
    addColumnarIndex();

    runQuerySortProj(BSONObj(), BSON("b" << 1), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                sort: {
                    pattern: {b: 1},
                    limit: 0,
                    node: {column_ixscan: {outputFields: ['a', 'b'], matchFields: []}}
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ExclusionProjectionDoesNotUseColumnarIndex) {
    addColumnarIndex();

    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 0 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 0, _id: 0}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, NoProjectionDoesNotUseColumnarIndex) {
    addColumnarIndex();

    runQuerySortProj(BSON("a" << 1), BSONObj(), BSONObj());
    assertNumSolutions(1U);
    assertSolutionExists(R"({cscan: {dir: 1, filter: {a: {$eq: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, ProjectionWithTooManyFieldsDoesNotUseColumnarIndex) {
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "b" << 1 << "c" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, c: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, ExpressionProjectionWithTooManyFieldsDoesnotUseColumnarIndex) {
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    // This will need 3 fields for the $concat, so should not be able to use a column scan.
    runQuerySortProj(BSONObj(), BSONObj(), fromjson("{str: {$concat: ['$a', '$b', '$c']}}"));
    assertNumSolutions(1U);
    assertSolutionExists(
        R"({proj: {spec: {str: {$concat: ['$a', '$b', '$c']}}, node: {cscan: {dir: 1}}}})");
}

// Test with a number of fields equal to the limit.
TEST_F(QueryPlannerColumnarTest, ImplicitIdCountsTowardsFieldLimit) {
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(
        BSONObj(), BSONObj(), BSON("a" << 1 << "b" << 1 /* _id implicitly included */));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, ProjectionWithJustEnoughFieldsDoesUseColumnarIndex) {
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    // Without the '_id' this should be eligible.
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "b" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"(
        {proj: {spec: {a: 1, b: 1, _id: 0}, node: {column_ixscan: {outputFields: ['a', 'b']}}}})");
}

TEST_F(QueryPlannerColumnarTest, DottedProjectionTooManyFieldsDoesNotUseColumnarIndex) {
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "b" << BSON("c" << 1 << "d" << 1)));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, 'b.c': 1, 'b.d': 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest,
       ProjectionWithTooManyFieldsDoesNotUseColumnarIndexUnsupportedPredicate) {
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan.store(2);
    runQuerySortProj(BSON("unsupported" << BSON("$exists" << false)),
                     BSONObj(),
                     BSON("a" << 1 << "b" << 1 << "c" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, c: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, StandardIndexPreferredOverColumnarIndex) {
    addColumnarIndex();
    addIndex(BSON("a" << 1));

    runQuerySortProj(BSON("a" << 5), BSONObj(), BSON("a" << 1 << "_id" << 0));

    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, _id: 0}, node: {ixscan: {pattern: {a: 1}}}}})");
}

TEST_F(QueryPlannerColumnarTest, IneligiblePredicateNeedsToBeAppliedAfterAssembly) {
    addColumnarIndex();

    runQuerySortProj(BSON("a" << BSONNULL), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_ixscan: {
                    filtersByPath: {},
                    outputFields: ['a'],
                    matchFields: ['a'],
                    postAssemblyFilter: {a: {$eq: null}}
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, MultiplePredicatesAllowedWithColumnarIndex) {
    addColumnarIndex();

    runQuerySortProj(BSON("a" << 2 << "b" << 3), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_ixscan: {
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
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(2);
    runQuerySortProj(BSON("a" << 2 << "b" << 3), BSONObj(), BSON("a" << 1 << "b" << 1 << "c" << 1));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, c: 1}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, TooManyFilteredFieldsDisqualifiesColumnScan) {
    addColumnarIndex();

    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(2);
    runQuerySortProj(BSON("a" << 2 << "b" << 3 << "c" << 4),
                     BSONObj(),
                     BSON("a" << 1 << "b" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {a: 1, b: 1, _id: 0}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, FilterDependingOnWholeDocumentDisqualifiesColumnScan) {
    addColumnarIndex();

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
    addColumnarIndex();

    // Neither the match nor the project mentions 4 fields, but together they exceed the threshhold.
    internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan.store(4);
    runQuerySortProj(BSON("a" << 2 << "b" << 3 << "c" << 4),
                     BSONObj(),
                     BSON("c" << 1 << "d" << 1 << "e" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({proj: {spec: {c: 1, d: 1, e: 1, _id: 0}, node: {cscan: {dir: 1}}}})");
}

TEST_F(QueryPlannerColumnarTest, NumberOfFieldsComputedUsingSetSize) {
    addColumnarIndex();

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
                column_ixscan: {
                    filtersByPath: {a: {a: {$eq: 2}}, b: {b: {$eq: 3}}, c: {c: {$eq: 4}}},
                    outputFields: ['a', 'b'],
                    matchFields: ['a', 'b', 'c']
                }
            }
        }
    })");
}
TEST_F(QueryPlannerColumnarTest, ComplexPredicateSplitDemo) {
    addColumnarIndex();

    auto complexPredicate = fromjson(R"({
        a: {$gte: 0, $lt: 10},
        "addresses.zip": {$in: ["12345", "01234"]},
        unsubscribed: false,
        specialAddress: {$exists: true}
    })");
    runQuerySortProj(complexPredicate, BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_ixscan: {
                    filtersByPath: {
                        a: {$and: [{a: {$gte: 0}}, {a: {$lt: 10}}]},
                        'addresses.zip': {'addresses.zip': {$in: ['12345', '01234']}},
                        unsubscribed: {unsubscribed: {$eq: false}},
                        specialAddress: {specialAddress: {$exists: true}}
                    },
                    outputFields: ['a'],
                    matchFields: ['a', 'addresses.zip', 'unsubscribed', 'specialAddress']
                }
            }
        }
    })");
}

TEST_F(QueryPlannerColumnarTest, ComplexPredicateSplitsIntoParts) {
    addColumnarIndex();

    // Same predicate as above, except with exists: false, which disqualifies the whole thing.
    auto complexPredicate = fromjson(R"({
        a: {$gte: 0, $lt: 10},
        "addresses.zip": {$in: ["12345", "01234"]},
        unsubscribed: false,
        specialAddress: {$exists: false},
        doNotContact: {$exists: false}
    })");
    runQuerySortProj(complexPredicate, BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {
                column_ixscan: {
                    filtersByPath: {
                        a: {a: {$gte: 0, $lt: 10}},
                        "addresses.zip": {"addresses.zip": {$in: ['12345', '01234']}},
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
    addColumnarIndex();

    runQuerySortProj(BSONObj(), BSONObj(), BSON("a" << 1 << "_id" << 0));
    assertNumSolutions(1U);
    assertSolutionExists(R"({
        proj: {
            spec: {a: 1, _id: 0},
            node: {column_ixscan: {filtersByPath: {}, outputFields: ['a'], matchFields: []}}
        }
    })");
}
}  // namespace mongo
