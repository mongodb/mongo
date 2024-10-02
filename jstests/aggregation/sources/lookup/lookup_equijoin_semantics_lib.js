/**
 * Tests for $lookup with localField/foreignField syntax.
 *
 * This file only specifies the tests. The tests are run for specific join algorithms via the three
 * `lookup_equijoin_semantics_*.js" files.
 *
 * In the classic engine these are NLJ and INLJ, and in SBE we also have HJ.
 *
 * The choice between indexed vs non-indexed joins is done based on the presence of an index on the
 * 'foreignField'. We test with three different types of indexes: sorted ascending, sorted
 * descending and hashed.
 *
 * The choice between HJ and NLJ is made based on the value of 'allowDiskUse' setting (because all
 * data in these tests is small and that enables HJ as long as 'allowDiskUse' is 'true').
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getAggPlanStages} from "jstests/libs/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";

export const JoinAlgorithm = {
    HJ: {name: "HJ", strategy: "HashJoin"},
    NLJ: {name: "NLJ", strategy: "NestedLoopJoin"},
    INLJ_Asc: {name: "INLJ_Asc", indexType: 1, strategy: "IndexedLoopJoin"},
    INLJ_Dec: {name: "INLJ_Dec", indexType: -1, strategy: "IndexedLoopJoin"},
    INLJ_Hashed: {name: "INLJ_Hashed", indexType: "hashed", strategy: "IndexedLoopJoin"}
};

export function setupCollections(testConfig, localRecords, foreignRecords, foreignField) {
    const {localColl, foreignColl, currentJoinAlgorithm} = testConfig;
    localColl.drop();
    assert.commandWorked(localColl.insert(localRecords));

    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecords));
    if (currentJoinAlgorithm.indexType) {
        const indexSpec = {[foreignField]: currentJoinAlgorithm.indexType};
        assert.commandWorked(foreignColl.createIndex(indexSpec));
    }
    // For NLJ and HJ do not create an index.
}

/**
 * Checks that the expected join algorithm has been used (to sanity check that the tests do provide
 * the intended coverage).
 */
export function checkJoinConfiguration(testConfig, explain) {
    const {currentJoinAlgorithm} = testConfig;
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");
    if (checkSbeRestrictedOrFullyEnabled(db)) {
        if (eqLookupNodes.length > 0) {
            // The $lookup stage has been lowered. Check that it's using the expected join strategy.
            assert.eq(currentJoinAlgorithm.strategy, eqLookupNodes[0].strategy, "Join strategy");
        } else {
            // In some suites, the tests cannot be lowered to SBE for reasons outside of our
            // control (e.g. sharding or pipeline being wrapped into other stages). We test the fact
            // that $lookup is lowered when we expect it to in lookup_pushdown.js.
            return false;
        }
    }
    return true;
}

/**
 * Executes $lookup with exactly one record in the foreign collection, so we don't need to check the
 * content of the "as" field but only that it's not empty for local records with ids in
 * 'idsExpectToMatch'.
 */
export function runTest_SingleForeignRecord(
    testConfig,
    {testDescription, localRecords, localField, foreignRecord, foreignField, idsExpectedToMatch}) {
    const {localColl, foreignColl, currentJoinAlgorithm} = testConfig;
    assert('object' === typeof (foreignRecord) && !Array.isArray(foreignRecord),
           "foreignRecord should be a single document");
    testDescription += ` (currentJoinAlgorithm: ${currentJoinAlgorithm.name})`;

    setupCollections(testConfig, localRecords, [foreignRecord], foreignField);

    const pipeline = [{
        $lookup: {
            from: foreignColl.getName(),
            localField: localField,
            foreignField: foreignField,
            as: "matched"
        }
    }];
    const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};

    const results = localColl.aggregate(pipeline, aggOptions).toArray();
    const explain = localColl.explain().aggregate(pipeline, aggOptions);

    // The foreign record should never duplicate in the results (e.g. see SERVER-66119). That is,
    // the "matched" field should either be an empty array or contain a single element.
    for (let i = 0; i < results.length; i++) {
        assert(results[i].matched.length < 2,
               testDescription + " Found duplicated match in " + tojson(results[i]));
    }

    // Build the array of ids for the results that have non-empty array in the "matched" field.
    const matchedIds = results
                           .filter(function(x) {
                               return tojson(x.matched) != tojson([]);
                           })
                           .map(x => (x._id));

    // Order of the elements within the arrays is not significant for 'assertArrayEq'.
    assertArrayEq({
        actual: matchedIds,
        expected: idsExpectedToMatch,
        extraErrorMsg: " **TEST** " + testDescription + " " + tojson(explain)
    });
}

/**
 * Executes $lookup with exactly one record in the local collection and checks that the "as" field
 * for it contains documents with ids from `idsExpectedToMatch`.
 */
export function runTest_SingleLocalRecord(
    testConfig,
    {testDescription, localRecord, localField, foreignRecords, foreignField, idsExpectedToMatch}) {
    const {localColl, foreignColl, currentJoinAlgorithm} = testConfig;
    assert('object' === typeof (localRecord) && !Array.isArray(localRecord),
           "localRecord should be a single document");
    testDescription += ` (currentJoinAlgorithm: ${currentJoinAlgorithm.name})`;

    setupCollections(testConfig, [localRecord], foreignRecords, foreignField);

    const pipeline = [{
        $lookup: {
            from: foreignColl.getName(),
            localField: localField,
            foreignField: foreignField,
            as: "matched"
        }
    }];
    const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};

    const results = localColl.aggregate(pipeline, aggOptions).toArray();
    const explain = localColl.explain().aggregate(pipeline, aggOptions);

    assert.eq(1, results.length);

    // Extract matched foreign ids from the "matched" field.
    const matchedIds = results[0].matched.map(x => (x._id));

    // Order of the elements within the arrays is not significant for 'assertArrayEq'.
    assertArrayEq({
        actual: matchedIds,
        expected: idsExpectedToMatch,
        extraErrorMsg: " **TEST** " + testDescription + " " + tojson(explain)
    });
}

/**
 * Executes $lookup and expects it to fail with the specified 'expectedErrorCode`.
 */
export function runTest_ExpectFailure(
    testConfig,
    {testDescription, localRecords, localField, foreignRecords, foreignField, expectedErrorCode}) {
    const {localColl, foreignColl, currentJoinAlgorithm} = testConfig;
    testDescription += ` (currentJoinAlgorithm: ${currentJoinAlgorithm.name})`;

    setupCollections(testConfig, localRecords, foreignRecords, foreignField);

    assert.commandFailedWithCode(
        localColl.runCommand("aggregate", {
            pipeline: [{
                $lookup: {
                    from: foreignColl.getName(),
                    localField: localField,
                    foreignField: foreignField,
                    as: "matched"
                }
            }],
            allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ,
            cursor: {}
        }),
        expectedErrorCode,
        "**TEST** " + testDescription);
}

/**
 * Tests.
 */
export function runTests(testConfig) {
    const {localColl, foreignColl, currentJoinAlgorithm} = testConfig;

    // Sanity-test that the join is configured correctly.
    setupCollections(testConfig, [{a: 1}], [{a: 1}], "a");
    const pipeline = [
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "a", as: "matched"}}
    ];
    const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};
    const explain = localColl.explain().aggregate(pipeline, aggOptions);
    if (!checkJoinConfiguration(testConfig, explain)) {
        // Some test suites execute these tests in a way that prevents $lookup from being lowered
        // into SBE (e.g. due to sharding, or wrapping the pipeline into $facet). We'll get coverage
        // for $lookup in the classic engine from running these tests in classic build variant so no
        // point running them multiple times.
        jsTestLog("Skipping tests because expected to lower $lookup to SBE but didn't");
        return;
    }

    (function testMatchingNullAndMissing() {
        const docs = [
            {_id: 0, no_a: 1},
            {_id: 1, a: null},
            {_id: 2, a: [null, 1]},

            {_id: 10, a: []},

            {_id: 20, a: 1},
            {_id: 21, a: {x: null}},
            {_id: 22, a: [[null, 1], 2]},
            {_id: 23, a: false},
            {_id: 24, a: 0},
            {_id: 25, a: {}},
            {_id: 26, a: ""}
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Null in foreign, top-level field in local",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: null},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 10]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Null in local, top-level field in foreign",
            localRecord: {_id: 0, b: null},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [0, 1, 2]
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Missing in foreign, top-level field in local",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, no_b: 1},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 10]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Missing in local, top-level field in foreign",
            localRecord: {_id: 0, no_b: 1},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [0, 1, 2]
        });
    })();

    (function testMatchingUndefined() {
        const docs = [
            {_id: 0, no_a: 1},
            {_id: 1, a: null},
            {_id: 2, a: []},
            {_id: 3, a: [null, 1]},

            {_id: 10, a: 1},
            {_id: 11, a: [[null, 1], 2]},
        ];

        // "undefined" should only match "undefined".
        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Undefined in foreign, top-level field in local",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: undefined},
            foreignField: "b",
            idsExpectedToMatch: []
        });

        // $lookup is allowed to run with sbe when internalQueryFrameworkControl is set to
        // 'trySbeRestricted'.
        if (checkSbeRestrictedOrFullyEnabled(db)) {
            // When lowered to SBE, "undefined" should only match "undefined".
            runTest_SingleForeignRecord(testConfig, {
                testDescription: "Undefined in foreign, undefined in array in local",
                localRecords: [{_id: 0, a: undefined}, {_id: 1, a: [undefined, 1]}],
                localField: "a",
                foreignRecord: {_id: 0, b: undefined},
                foreignField: "b",
                idsExpectedToMatch: [0, 1]
            });
            runTest_SingleLocalRecord(testConfig, {
                testDescription: "Undefined in local, undefined in array in foreign",
                localRecord: {_id: 0, b: undefined},
                localField: "a",
                foreignRecords: [{_id: 0, a: undefined}, {_id: 1, a: [undefined, 1]}],
                foreignField: "b",
                idsExpectedToMatch: [0, 1]
            });

            runTest_SingleLocalRecord(testConfig, {
                testDescription: "Undefined in local, top-level field in foreign",
                localRecord: {_id: 0, b: undefined},
                localField: "b",
                foreignRecords: docs,
                foreignField: "a",
                idsExpectedToMatch: []
            });
        } else {
            // Due to legacy reasons, in the classic engine if the left-hand side collection has a
            // value of undefined for "localField", then the query will fail. This is a consequence
            // of the fact that queries which explicitly compare to undefined, such as
            // {$eq:undefined}, are banned. Arguably this behavior could be improved, but we are
            // unlikely to change it given that the undefined BSON type has been deprecated for many
            // years.
            runTest_ExpectFailure(testConfig, {
                testDescription: "Undefined in local, top-level field in foreign",
                localRecords: {_id: 0, b: undefined},
                localField: "b",
                foreignRecords: docs,
                foreignField: "a",
                expectedErrorCode: ErrorCodes.BadValue
            });
        }
    })();

    (function testMatchingNaN() {
        const docs = [
            {_id: 0, a: NaN},
            {_id: 1, a: NumberDecimal("NaN")},
            {_id: 2, a: [1, NaN]},
            {_id: 3, a: [1, NumberDecimal("NaN")]},

            {_id: 10, a: null},
            {_id: 11, no_a: 42},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "NaN in foreign, top-level field in local",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: NaN},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "NaN in local, top-level field in foreign",
            localRecord: {_id: 0, b: NaN},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [0, 1, 2, 3]
        });
    })();

    (function testVariousDataTypes(indexType) {
        // NOTE: There is no shell equivalent for the following BSON types:
        // - Code (13)
        // - Symbol (14)
        // - CodeWScope (15)
        const docs = [
            {_id: 0, a: NumberInt(0)},
            {_id: 1, a: 3.14},
            {_id: 2, a: NumberDecimal(3.14)},
            {_id: 3, a: "abc"},
            {_id: 4, a: {b: 1, c: 2, d: 3}},
            {_id: 5, a: true},
            {_id: 6, a: false},
            {_id: 7, a: new ISODate("2022-01-01T00:00:00.00Z")},
            {_id: 8, a: new Timestamp(1, 123)},
            {_id: 9, a: new ObjectId("0102030405060708090A0B0C")},
            {_id: 10, a: new BinData(0, "BBBBBBBBBBBBBBBBBBBBBBBBBBBB")},
            {_id: 11, a: /hjkl/},
            {_id: 12, a: /hjkl/g},
            {_id: 13, a: new DBRef("collection", "id", "database")},
        ];

        docs.forEach(doc => {
            runTest_SingleForeignRecord(testConfig, {
                testDescription: "Various data types in local matching to: " + tojson(doc),
                localRecords: docs,
                localField: "a",
                foreignRecord: {b: doc.a},
                foreignField: "b",
                idsExpectedToMatch: [doc._id]
            });
            runTest_SingleLocalRecord(testConfig, {
                testDescription: "Various data types in foreign matching to: " + tojson(doc),
                localRecord: {b: doc.a},
                localField: "b",
                foreignRecords: docs,
                foreignField: "a",
                idsExpectedToMatch: [doc._id]
            });
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Various data types in local, expecting no match",
            localRecords: docs,
            localField: "a",
            foreignRecord: {b: 'xxx'},
            foreignField: "b",
            idsExpectedToMatch: []
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Various data types in foreign, expecting no match",
            localRecord: {b: 'xxx'},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: []
        });
    })();

    (function testMatchingTopLevelFieldToScalar() {
        const docs = [
            // For these docs "a" resolves to a (logical) set that contains value "1".
            {_id: 0, a: 1, y: 2},
            {_id: 1, a: [1]},
            {_id: 2, a: [1, 2, 3]},
            {_id: 3, a: [1, [2, 3]]},
            {_id: 4, a: [1, [1, 2]]},
            {_id: 5, a: [1, 2, 1]},
            {_id: 6, a: [1, null]},
            {_id: 7, a: [1, []]},

            // For these docs "a" resolves to a (logical) set that does _not_ contain value "1".
            {_id: 10, a: 2},
            {_id: 11, a: [[1], 2]},
            {_id: 12, a: [2, 3]},
            {_id: 13, a: {y: 1}},
            {_id: 14, no_a: 1},
        ];

        // When matching a scalar, local and foreign collections are fully symmetric.
        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Top-level field in local and top-level scalar in foreign",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: 1},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level scalar in local and top-level field in foreign",
            localRecord: {_id: 0, b: 1},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
        });
    })();

    (function testMatchingPathToScalar() {
        const docs = [
            // For these docs "a.x" resolves to a (logical) set that contains value "1".
            {_id: 0, a: {x: 1, y: 2}},
            {_id: 1, a: [{x: 1}, {x: 2}]},
            {_id: 2, a: [{x: 1}, {x: null}]},
            {_id: 3, a: [{x: 1}, {x: []}]},
            {_id: 4, a: [{x: 1}, {no_x: 2}]},
            {_id: 5, a: {x: [1, 2]}},
            {_id: 6, a: [{x: [1, 2]}]},
            {_id: 7, a: [{x: [1, 2]}, {no_x: 2}]},

            // For these docs "a.x" should resolve to a (logical) set that does _not_ contain value
            // "1".
            {_id: 10, a: {x: 2, y: 1}},
            {_id: 11, a: {x: [2, 3], y: 1}},
            {_id: 12, a: [{no_x: 1}, {x: 2}, {x: 3}]},
            {_id: 13, a: {x: [[1], 2]}},
            {_id: 14, a: [{x: [[1], 2]}]},
            {_id: 15, a: {no_x: 1}},
        ];

        // When matching a scalar, local and foreign collections are fully symmetric.
        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Path in local and top-level scalar in foreign",
            localRecords: docs,
            localField: "a.x",
            foreignRecord: {_id: 0, b: 1},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level scalar in local and path in foreign",
            localRecord: {_id: 0, b: 1},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.x",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
        });
    })();

    (function testMatchingDeepPathToScalar() {
        const docs = [
            // For these docs "a.b.c" resolves to a (logical) set that contains value "1" (and
            // possibly other values)
            {_id: 0, a: {b: {c: 1}}},
            {_id: 1, a: {b: {c: [[2, 3], 1]}}},

            {_id: 2, a: {b: [{c: 2}, {c: 1}]}},
            {_id: 3, a: {b: [{c: null}, {c: 1}]}},
            {_id: 4, a: {b: [{no_c: 2}, {c: 1}]}},
            {_id: 5, a: {b: [{c: []}, {c: 1}]}},
            {_id: 6, a: {b: [{c: [[2, 3], 1]}]}},
            {_id: 7, a: {b: [{c: 1}, {c: [[2, 3], 4]}]}},

            {_id: 8, a: [{b: {c: 2}}, {b: {c: 1}}]},
            {_id: 9, a: [{b: {c: null}}, {b: {c: 1}}]},
            {_id: 10, a: [{b: {no_c: 2}}, {b: {c: 1}}]},
            {_id: 11, a: [{b: {c: []}}, {b: {c: 1}}]},
            {_id: 12, a: [{b: {c: [[2, 3], 1]}}]},
            {_id: 13, a: [{b: {c: 4}}, {b: {c: [[2, 3], 1]}}]},
            {_id: 14, a: [{no_b: 2}, {b: {c: 1}}]},

            {_id: 15, a: [{b: [{c: 1}]}]},
            {_id: 16, a: [{b: {c: 3}}, {b: [{c: 1}, {c: 2}]}]},
            {_id: 17, a: [{b: {c: null}}, {b: [{c: 1}]}]},
            {_id: 18, a: [{b: {no_c: 2}}, {b: [{c: 1}]}]},
            {_id: 19, a: [{b: {c: []}}, {b: [{c: 1}]}]},
            {_id: 20, a: [{b: [{c: [[2, 3], 1]}]}]},
            {_id: 21, a: [{b: {c: 4}}, {b: [{c: [[2, 3], 1]}]}]},
            {_id: 22, a: [{no_b: 2}, {b: [{no_c: 3}, {c: 1}]}]},

            // For these docs "a.b.c" should resolve to a (logical) set that does _not_ contain
            // value "1" (but might contain other values).
            {_id: 100, a: {b: {c: [[1, 2], 3]}}},
            {_id: 101, a: {b: [{c: [[1, 2], 3]}]}},
            {_id: 102, a: [{b: {c: [[1, 2], 3]}}]},
            {_id: 103, a: [{b: [{c: [[1, 2], 3]}]}]},
        ];

        // When matching a scalar, local and foreign collections are fully symmetric.
        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Deep path in local and top-level scalar in foreign",
            localRecords: docs,
            localField: "a.b.c",
            foreignRecord: {_id: 0, key: 1},
            foreignField: "key",
            idsExpectedToMatch:
                [0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level scalar in local and deep path in foreign",
            localRecord: {_id: 0, key: 1},
            localField: "key",
            foreignRecords: docs,
            foreignField: "a.b.c",
            idsExpectedToMatch:
                [0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22]
        });
    })();

    (function testMatchingTopLevelFieldToArray() {
        const docs = [
            // For these docs "a" resolves to a (logical) set that contains [1,2] array as a value.
            {_id: 0, a: [[1, 2], 3], y: 4},
            {_id: 1, a: [[1, 2]]},

            // For these docs "a.x" contains [1,2], 1 and 2 values when in foreign, but in local
            // the contained values are 1 and 2 (no array).
            {_id: 2, a: [1, 2], y: 3},

            // For these docs "a" resolves to a (logical) set that does _not_ contain [1,2] as a
            // value in neither local nor foreign but might contain "1" and/or "2".
            {_id: 10, a: [[[1, 2], 3], 4]},
            {_id: 11, a: [2, 1]},
            {_id: 12, a: [[2, 1], 3], y: [1, 2]},
            {_id: 13, a: [[2, 1], 3], y: [[1, 2], 3]},
            {_id: 14, a: null},
            {_id: 15, no_a: [1, 2]},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Top-level field in local and top-level array in foreign",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: [1, 2]},
            foreignField: "b",
            idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, /*match on 1: */ 2, 11]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level array in local and top-level field in foreign",
            localRecord: {_id: 0, b: [1, 2]},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [/*match on 1: */ 2, 11]
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Top-level field in local and nested array in foreign",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: [[1, 2], 42]},
            foreignField: "b",
            idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Nested array in local and top-level field in foreign",
            localRecord: {_id: 0, b: [[1, 2], 42]},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, 2]
        });
    })();

    (function testMatchingPathToArray() {
        const docs = [
            // For these docs "a.x" resolves to a (logical) set that contains [1,2] array as a
            // value.
            {_id: 0, a: {x: [[1, 2], 3], y: 4}},
            {_id: 1, a: [{x: [[1, 2], 3]}, {x: 4}]},
            {_id: 2, a: [{x: [[1, 2], 3]}, {x: null}]},
            {_id: 3, a: [{x: [[1, 2], 3]}, {no_x: 4}]},

            // For these docs "a.x" contains [1,2], 1 and 2 values when in foreign, but in local
            // the contained values are 1 and 2 (no array).
            {_id: 4, a: {x: [1, 2], y: 4}},
            {_id: 5, a: [{x: [1, 2]}, {x: 4}]},
            {_id: 6, a: [{x: [1, 2]}, {x: null}]},
            {_id: 7, a: [{x: [1, 2]}, {no_x: 4}]},

            // For these docs "a.x" resolves to a (logical) set that doesn't contain [1,2] as a
            // value in neither local nor foreign but might contain "1" and/or "2".
            {_id: 10, a: {x: [2, 1], y: [1, 2]}},
            {_id: 11, a: {x: [[2, 1], 3], y: [[1, 2], 3]}},
            {_id: 12, a: [{x: 1}, {x: 2}]},
            {_id: 13, a: {x: [[[1, 2], 3]]}},
            {_id: 14, a: [{x: [[[1, 2], 3]]}]},
            {_id: 15, a: {no_x: [[1, 2], 3]}},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Path in local and top-level array in foreign",
            localRecords: docs,
            localField: "a.x",
            foreignRecord: {_id: 0, b: [1, 2]},
            foreignField: "b",
            idsExpectedToMatch:
                [/*match on [1, 2]: */ 0, 1, 2, 3, /*match on 1: */ 4, 5, 6, 7, 10, 12]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level array in local and path in foreign",
            localRecord: {_id: 0, b: [1, 2]},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.x",
            idsExpectedToMatch: [/*match on 1: */ 4, 5, 6, 7, 10, 12]
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Path in local and nested array in foreign",
            localRecords: docs,
            localField: "a.x",
            foreignRecord: {_id: 0, b: [[1, 2], 42]},
            foreignField: "b",
            idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, 2, 3]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Nested array in local and path in foreign",
            localRecord: {_id: 0, b: [[1, 2], 42]},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.x",
            idsExpectedToMatch: [/*match on [1, 2]: */ 0, 1, 2, 3, 4, 5, 6, 7]
        });
    })();

    (function testMatchingMissingOnPath() {
        const docs = [
            // "a.x" does not exist.
            {_id: 0, a: {no_x: 1}},
            {_id: 1, a: {no_x: [1, 2]}},
            {_id: 2, a: [{no_x: 1}, {no_x: 2}]},
            {_id: 3, a: [{no_x: 2}, {x: 1}]},
            {_id: 4, a: [{no_x: [1, 2]}, {x: 1}]},
            {_id: 5, a: {x: null}},
            {_id: 6, a: [{x: null}, {x: 1}]},
            {_id: 7, a: [{x: null}, {x: [1]}]},
            {_id: 8, no_a: 1},
            {_id: 9, a: [1]},
            {_id: 10, a: []},
            {_id: 11, a: [[1]]},
            {_id: 12, a: [[]]},

            // "a.x" exists.
            {_id: 20, a: {x: 2, y: 1}},
            {_id: 21, a: [{x: 2}, {x: 3}]},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Missing in local path and top-level null in foreign",
            localRecords: docs,
            localField: "a.x",
            foreignRecord: {_id: 0, b: null},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level null in local and missing in foreign path",
            localRecord: {_id: 0, b: null},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.x",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7, 8]
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Missing in local path and top-level missing in foreign",
            localRecords: docs,
            localField: "a.x",
            foreignRecord: {_id: 0, no_b: 1},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 5, 6, 7, 8, 9, 10, 11, 12]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level missing in local and missing in foreign path",
            localRecord: {_id: 0, no_b: 1},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.x",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7, 8]
        });
    })();

    (function testMatchingScalarsOnPath() {
        const docs = [
            {_id: 0, a: 1},
            {_id: 1, no_a: 1},
            {_id: 2, a: [1]},

            {_id: 3, a: {b: 1}},
            {_id: 4, a: {no_b: 1}},
            {_id: 5, a: {b: [1]}},

            {_id: 6, a: [{b: {no_c: 1}}, 1]},
            {_id: 7, a: {b: [{no_c: 1}, 1]}},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Scalars on local path and top-level null in foreign",
            localRecords: docs,
            localField: "a.b.c",
            foreignRecord: {_id: 0, b: null},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6, 7]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Top-level null in local and scalars on path in foreign",
            localRecord: {_id: 0, b: null},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.b.c",
            idsExpectedToMatch: [0, 1, 3, 4, 6, 7]
        });
    })();

    (function testMatchingEmptyArrayInTopLevelField() {
        const docs = [
            // For these docs "a" resolves to a (logical) set that contains empty array as a value.
            {_id: 0, a: [[]]},
            {_id: 1, a: [[], 1]},
            {_id: 2, a: [[], null]},

            // For these docs "a" resolves to a (logical) set that contains empty array as a value
            // in foreign collection only.
            {_id: 3, a: []},

            // For these docs "a" key is either missing or contains null.
            {_id: 10, no_a: 1},
            {_id: 11, a: null},
            {_id: 12, a: [null]},
            {_id: 13, a: [null, 1]},

            // "a" contains neither empty array nor null.
            {_id: 20, a: 1},
            {_id: 21, a: [[[], 1], 2]},
            {_id: 22, a: [1, 2]},
            {_id: 23, a: [[null, 1], 2]},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Empty top-level array in foreign, top field in local",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: []},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Empty top-level array in local, top field in foreign",
            localRecord: {_id: 0, b: []},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [2, 10, 11, 12, 13]
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Empty nested array in foreign, top field in local",
            localRecords: docs,
            localField: "a",
            foreignRecord: {_id: 0, b: [[], 42]},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Empty nested array in local, top field in foreign",
            localRecord: {_id: 0, b: [[], 42]},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: [0, 1, 2, 3]
        });
    })();

    (function testMatchingDeepPathWithEmptyArrays() {
        const docs = [
            {_id: 0, a: {b: {c: []}}},
            {_id: 1, a: {b: [{c: []}, {c: []}]}},
            {_id: 2, a: {b: [{no_c: 42}, {c: []}, {c: []}]}},
            {_id: 3, a: [{b: {c: []}}, {b: {c: []}}]},
            {_id: 4, a: [{b: {no_c: 42}}, {b: {c: []}}, {b: {c: []}}]},

            {_id: 5, a: {b: {c: [[]]}}},
            {_id: 6, a: {b: [{c: []}, {c: [[]]}]}},
            {_id: 7, a: {b: [{no_c: 42}, {c: []}, {c: [[]]}]}},
            {_id: 8, a: [{b: {c: []}}, {b: {c: [[]]}}]},
            {_id: 9, a: [{b: {no_c: 42}}, {b: {c: []}}, {b: {c: [[]]}}]},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Empty arrays and missing on deep path in local",
            localRecords: docs,
            localField: "a.b.c",
            foreignRecord: {_id: 0, b: null},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3, 4]
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Empty arrays and missing on deep path in local",
            localRecords: docs,
            localField: "a.b.c",
            foreignRecord: {_id: 0, b: []},
            foreignField: "b",
            idsExpectedToMatch: [5, 6, 7, 8, 9]
        });
    })();

    (function testMatchingEmptyArrayValueOnPath() {
        const docs = [
            // For these docs "a.x" resolves to a (logical) set that contains empty array as a
            // value.
            {_id: 0, a: {x: [[]], y: 1}},
            {_id: 1, a: [{x: [[]]}]},
            {_id: 2, a: [{x: [[]]}, {x: 1}]},
            {_id: 3, a: [{x: [[]]}, {x: null}]},
            {_id: 4, a: [{x: [[]]}, {no_x: 1}]},
            {_id: 5, a: {x: [[], 1]}},

            // For these docs "a.x" resolves to a (logical) set that contains empty array as a value
            // in foreign collection only.
            {_id: 10, a: {x: [], y: 1}},
            {_id: 11, a: [{x: []}, {x: 1}]},
            {_id: 12, a: [{x: []}, {x: null}]},
            {_id: 13, a: [{x: []}, {no_x: 1}]},

            // For these docs "a.x" key is either missing or contains null.
            {_id: 20, no_a: 1},
            {_id: 21, a: {no_x: 1}},
            {_id: 22, a: [{no_x: 1}, {no_x: 2}]},
            {_id: 23, a: [{x: null}, {x: 1}]},

            {_id: 30, a: []},
            {_id: 31, a: [1]},
            {_id: 32, a: [null, 1]},
            {_id: 33, a: [[]]},
            {_id: 34, a: [[1], 2]},

            // "a.x" doesn't contain neither empty array nor null.
            {_id: 40, a: {x: 1}},
            {_id: 41, a: {x: [[[], 1], 2]}},
            {_id: 42, a: {x: [1, 2]}},
            {_id: 43, a: {x: [[null, 1], 2]}},
            {_id: 44, a: [{x: 1}]},
            {_id: 45, a: [{x: [[[], 1], 2]}]},
            {_id: 46, a: [{x: [1, 2]}]},
            {_id: 47, a: [{x: [[null, 1], 2]}]},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Empty top-level array in foreign, path in local",
            localRecords: docs,
            localField: "a.x",
            foreignRecord: {_id: 0, b: []},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Empty top-level array in local, path in foreign",
            localRecord: {_id: 0, b: []},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.x",
            idsExpectedToMatch: [3, 4, 12, 13, 20, 21, 22, 23]
        });

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Empty nested array in foreign, path in local",
            localRecords: docs,
            localField: "a.x",
            foreignRecord: {_id: 0, b: [[], 42]},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Empty nested array in local, path in foreign",
            localRecord: {_id: 0, b: [[], 42]},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.x",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 10, 11, 12, 13]
        });
    })();

    (function testMatchingPathWithNumericComponentToScalar() {
        const docs = [
            // For these docs "a.0.x" resolves to a (logical) set that contains value "1".
            {_id: 0, a: [{x: 1}, {x: 2}]},
            {_id: 1, a: [{x: [2, 3, 1]}]},
            {_id: 2, a: [{x: 1}, {y: 1}]},

            // For these docs "a.0.x" resolves to a (logical) set that does _not_ contain value "1".
            {_id: 10, a: [{x: 2}, {x: 1}]},
            {_id: 11, a: [{x: [2, 3]}]},
            {_id: 12, a: {x: 1}},
            {_id: 13, a: {x: [1, 2]}},
            {_id: 14, a: [{y: 1}, {x: 1}]},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Scalar in foreign, path with numeral in local",
            localRecords: docs,
            localField: "a.0.x",
            foreignRecord: {_id: 0, b: 1},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2]
        });
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Scalar in local, path with numeral in foreign",
            localRecord: {_id: 0, b: 1},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.0.x",
            idsExpectedToMatch: [0, 1, 2]
        });
    })();

    (function testMatchingPathWithNumericComponentToNull() {
        const docs = [
            // For these docs "a.0.x" resolves to a (logical) set that contains value "null".
            {_id: 0, a: {x: 1}},
            {_id: 1, a: {x: [1, 2]}},
            {_id: 2, a: [{y: 1}, {x: 1}]},
            {_id: 3, a: [{x: null}, {x: 1}]},
            {_id: 4, a: [{x: [1, null]}, {x: 1}]},
            {_id: 5, a: [1, 2]},

            // For these docs "a.0.x" resolves to a (logical) set that does _not_ contain value
            // "null".
            {_id: 10, a: [{x: 1}, {y: 1}]},
            {_id: 11, a: [{x: [1, 2]}]},
        ];

        runTest_SingleForeignRecord(testConfig, {
            testDescription: "Null in foreign, path with numeral in local",
            localRecords: docs,
            localField: "a.0.x",
            foreignRecord: {_id: 0, b: null},
            foreignField: "b",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5]
        });
        // SERVER-64221/SERVER-27442: matching to null isn't consistent.
        const S64221 =
            (currentJoinAlgorithm == JoinAlgorithm.NLJ || currentJoinAlgorithm == JoinAlgorithm.HJ)
            ? [10, 11]
            : [];
        runTest_SingleLocalRecord(testConfig, {
            testDescription: "Null in local, path with numeral in foreign",
            localRecord: {_id: 0, b: null},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a.0.x",
            idsExpectedToMatch: [0, 1, 2, 3, 4].concat(S64221)
        });
    })();

    /**
     * Executes $lookup with non existent foreign collection and checks that the "as" field for it
     * contains empty arrays.
     */
    (function testNonExistentForeignCollection() {
        localColl.drop();
        const localDocs = Array(10).fill({a: 1});
        assert.commandWorked(localColl.insert(localDocs));

        foreignColl.drop();

        const results = localColl.aggregate([{
            $lookup: {
                from: foreignColl.getName(),
                localField: "a",
                foreignField: "b",
                as: "matched"
            }
        }]).toArray();

        assert.eq(localDocs.length, results.length);

        // Local record should have no match.
        assert.eq(results[0].matched, []);
    })();
}
