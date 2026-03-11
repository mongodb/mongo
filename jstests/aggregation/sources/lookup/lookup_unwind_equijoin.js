/**
 * Tests for $lookup-$unwind with localField/foreignField syntax.
 *
 * The tests are run for specific join algorithms. In the classic engine these are NLJ and INLJ,
 * and in SBE we also have HJ.
 *
 * The choice between indexed vs non-indexed joins is done based on the presence of an index on the
 * 'foreignField'. We test with three different types of indexes: sorted ascending, sorted
 * descending and hashed.
 *
 * The choice between HJ and NLJ is made based on the value of 'allowDiskUse' setting (because all
 * data in these tests is small and that enables HJ as long as 'allowDiskUse' is 'true').
 */

import {JoinAlgorithm, setupCollections} from "jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

// No-op if 'currentJoinAlgorithm' is not INLJ.
function createIndex(currentJoinAlgorithm, coll, field) {
    if (currentJoinAlgorithm.indexType) {
        const indexSpec = {[field]: currentJoinAlgorithm.indexType};
        assert.commandWorked(coll.createIndex(indexSpec));
    }
}

function runTest(
    testConfig,
    {
        testDescription,
        localRecords,
        localField,
        foreignRecords,
        foreignField,
        preserveNullAndEmptyArrays,
        matches,
        joinPath = "matched",
        indexPath = "index",
    },
) {
    const {localColl, foreignColl, currentJoinAlgorithm} = testConfig;
    testDescription += ` (joinAlgorithm: ${currentJoinAlgorithm.name})`;

    setupCollections(testConfig, localRecords, foreignRecords, foreignField);

    const pipeline = [
        {
            $lookup: {
                from: foreignColl.getName(),
                localField: localField,
                foreignField: foreignField,
                as: joinPath,
            },
        },
        {
            $unwind: {
                path: `\$${joinPath}`,
                preserveNullAndEmptyArrays: preserveNullAndEmptyArrays,
                includeArrayIndex: indexPath,
            },
        },
    ];
    const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};

    const results = localColl.aggregate(pipeline, aggOptions).toArray();
    const explain = localColl.explain().aggregate(pipeline, aggOptions);

    function getNestedValue(obj, path) {
        return path.split(".").reduce((current, key) => current?.[key], obj);
    }

    // Build the array of ids for the results.
    const matchedIds = results
        // -1 is used for no match.
        .map((x) => [x._id, x.matched ? x.matched._id : -1]);

    // Build the array of indexes (includeArrayIndex) for the results.
    // Note: A separate array is used because the order of the matches is not guaranteed.
    const indexIds = results.map((x) => [x._id, getNestedValue(x, indexPath)]);

    // Order of the elements within the arrays is not significant for 'assertArrayEq'.
    assertArrayEq({
        actual: matchedIds,
        expected: matches.map((x) => [x[0], x[1]]),
        extraErrorMsg: " **TEST** " + testDescription + " " + tojson(explain),
    });
    assertArrayEq({
        actual: indexIds,
        expected: matches.map((x) => [x[0], x[2]]),
        extraErrorMsg: " **TEST** " + testDescription + " " + tojson(explain),
    });
}

/**
 * Tests.
 */
function runTests(testConfig) {
    const {localColl, foreignColl, currentJoinAlgorithm} = testConfig;

    function testMatchingNullAndVariousDataTypes() {
        const testDescription = "Matching null and various data types";
        const localDocs = [
            {_id: 0, no_a: 1},
            {_id: 1, a: null},
            {_id: 2, a: 42},
            {_id: 3, a: [1, 2, 3]},
            {_id: 4, a: "foo"},
            {_id: 5, a: "bar"},
        ];

        const foreignDocs = [
            {_id: 0, b: null},
            {_id: 1, no_b: 1},
            {_id: 2, b: 42},
            {_id: 3, b: 2},
            {_id: 4, b: [1, 42, 3]},
            {_id: 5, b: "foo"},
            {_id: 6, b: "baz"},
        ];

        runTest(testConfig, {
            testDescription: testDescription,
            localRecords: localDocs,
            localField: "a",
            foreignRecords: foreignDocs,
            foreignField: "b",
            preserveNullAndEmptyArrays: false,
            matches: [
                [0, 0, 0],
                [0, 1, 1],
                [1, 0, 0],
                [1, 1, 1],
                [2, 2, 0],
                [2, 4, 1],
                [3, 3, 0],
                [3, 4, 1],
                [4, 5, 0],
            ],
        });
        runTest(testConfig, {
            testDescription: `${testDescription}, preserveNullAndEmptyArrays=true`,
            localRecords: localDocs,
            localField: "a",
            foreignRecords: foreignDocs,
            foreignField: "b",
            preserveNullAndEmptyArrays: true,
            matches: [
                [0, 0, 0],
                [0, 1, 1],
                [1, 0, 0],
                [1, 1, 1],
                [2, 2, 0],
                [2, 4, 1],
                [3, 3, 0],
                [3, 4, 1],
                [4, 5, 0],
                [5, -1, null],
            ],
        });
        runTest(testConfig, {
            testDescription: `${testDescription}. Reverse local and foreign collections`,
            localRecords: foreignDocs,
            localField: "b",
            foreignRecords: localDocs,
            foreignField: "a",
            preserveNullAndEmptyArrays: false,
            matches: [
                [0, 0, 0],
                [0, 1, 1],
                [1, 0, 0],
                [1, 1, 1],
                [2, 2, 0],
                [3, 3, 0],
                [4, 2, 0],
                [4, 3, 1],
                [5, 4, 0],
            ],
        });
        runTest(testConfig, {
            testDescription: `${testDescription}. Reverse local and foreign collections, preserveNullAndEmptyArrays=true`,
            localRecords: foreignDocs,
            localField: "b",
            foreignRecords: localDocs,
            foreignField: "a",
            preserveNullAndEmptyArrays: true,
            matches: [
                [0, 0, 0],
                [0, 1, 1],
                [1, 0, 0],
                [1, 1, 1],
                [2, 2, 0],
                [3, 3, 0],
                [4, 2, 0],
                [4, 3, 1],
                [5, 4, 0],
                [6, -1, null],
            ],
        });
        // Join path is prefix of index path. Index is part of the matched foreign document.
        runTest(testConfig, {
            testDescription: `${testDescription}. `,
            localRecords: localDocs,
            localField: "a",
            foreignRecords: foreignDocs,
            foreignField: "b",
            preserveNullAndEmptyArrays: false,
            indexPath: "matched.index",
            matches: [
                [0, 0, 0],
                [0, 1, 1],
                [1, 0, 0],
                [1, 1, 1],
                [2, 2, 0],
                [2, 4, 1],
                [3, 3, 0],
                [3, 4, 1],
                [4, 5, 0],
            ],
        });
        // Index path is prefix of join path. Index overwrites matched foreign document.
        runTest(testConfig, {
            testDescription: `${testDescription}. `,
            localRecords: localDocs,
            localField: "a",
            foreignRecords: foreignDocs,
            foreignField: "b",
            preserveNullAndEmptyArrays: false,
            joinPath: "index.matched",
            matches: [
                [0, -1, 0],
                [0, -1, 1],
                [1, -1, 0],
                [1, -1, 1],
                [2, -1, 0],
                [2, -1, 1],
                [3, -1, 0],
                [3, -1, 1],
                [4, -1, 0],
            ],
        });
    }

    function testMissingLocalField() {
        const localDocs = [
            {_id: 0, no_a: 1},
            {_id: 1, a: 42},
        ];

        const foreignDocs = [{_id: 1, b: 0}];

        runTest(testConfig, {
            testDescription: "Missing in local, top-level field in foreign",
            localRecords: localDocs,
            localField: "a",
            foreignRecords: foreignDocs,
            foreignField: "b",
            preserveNullAndEmptyArrays: false,
            matches: [],
        });
        runTest(testConfig, {
            testDescription: "Missing in local, top-level field in foreign. preserveNullAndEmptyArrays=true",
            localRecords: localDocs,
            localField: "a",
            foreignRecords: foreignDocs,
            foreignField: "b",
            preserveNullAndEmptyArrays: true,
            matches: [
                [0, -1, null],
                [1, -1, null],
            ],
        });
    }

    function testMultiJoinOnLocalCollection() {
        function testRunner(matchedDocs, preserveNullAndEmptyArrays) {
            localColl.drop();
            const foreignColl1 = db.foreignColl1;
            foreignColl1.drop();
            const foreignColl2 = db.foreignColl2;
            foreignColl2.drop();
            const localDocs = [
                {_id: 0, a: 0},
                {_id: 1, a: 1},
                {_id: 2, a: 2},
                {_id: 3, a: 42},
            ];
            const foreignDocs1 = [
                {_id: 0, b: 0},
                {_id: 1, b: 1},
            ];
            const foreignDocs2 = [
                {_id: 0, c: 0},
                {_id: 2, c: 2},
            ];
            assert.commandWorked(localColl.insert(localDocs));
            assert.commandWorked(foreignColl1.insert(foreignDocs1));
            assert.commandWorked(foreignColl2.insert(foreignDocs2));
            createIndex(currentJoinAlgorithm, foreignColl1, "b");
            createIndex(currentJoinAlgorithm, foreignColl2, "c");

            const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};
            const results = localColl
                .aggregate(
                    [
                        {
                            $lookup: {
                                from: foreignColl1.getName(),
                                localField: "a",
                                foreignField: "b",
                                as: "foreign1",
                            },
                        },
                        {
                            $unwind: {
                                path: "$foreign1",
                                preserveNullAndEmptyArrays: preserveNullAndEmptyArrays,
                                includeArrayIndex: "index1",
                            },
                        },
                        {
                            $lookup: {
                                from: foreignColl2.getName(),
                                localField: "a",
                                foreignField: "c",
                                as: "foreign2",
                            },
                        },
                        {
                            $unwind: {
                                path: "$foreign2",
                                preserveNullAndEmptyArrays: preserveNullAndEmptyArrays,
                                includeArrayIndex: "index2",
                            },
                        },
                    ],
                    aggOptions,
                )
                .toArray();

            assertArrayEq({
                actual: results,
                expected: matchedDocs,
            });
        }
        testRunner(
            [
                {
                    "_id": 0,
                    "a": 0,
                    "foreign1": {
                        "_id": 0,
                        "b": 0,
                    },
                    "index1": 0,
                    "foreign2": {
                        "_id": 0,
                        "c": 0,
                    },
                    "index2": 0,
                },
            ],
            false /*preserveNullAndEmptyArrays*/,
        );
        testRunner(
            [
                {
                    "_id": 0,
                    "a": 0,
                    "foreign1": {
                        "_id": 0,
                        "b": 0,
                    },
                    "index1": 0,
                    "foreign2": {
                        "_id": 0,
                        "c": 0,
                    },
                    "index2": 0,
                },
                {
                    "_id": 1,
                    "a": 1,
                    "foreign1": {
                        "_id": 1,
                        "b": 1,
                    },
                    "index1": 0,
                    "index2": null,
                },
                {
                    "_id": 2,
                    "a": 2,
                    "index1": null,
                    "foreign2": {
                        "_id": 2,
                        "c": 2,
                    },
                    "index2": 0,
                },
                {
                    "_id": 3,
                    "a": 42,
                    "index1": null,
                    "index2": null,
                },
            ],
            true /*preserveNullAndEmptyArrays*/,
        );
    }

    function testMultiJoinForeignOnForeign() {
        function testRunner(matchedDocs, preserveNullAndEmptyArrays) {
            localColl.drop();
            const foreignColl1 = db.foreignColl1;
            foreignColl1.drop();
            const foreignColl2 = db.foreignColl2;
            foreignColl2.drop();
            const localDocs = [
                {_id: 0, a: 0},
                {_id: 1, a: 1},
                {_id: 2, a: 42},
            ];
            const foreignDocs1 = [
                {_id: 0, b: 0},
                {_id: 1, b: 1},
            ];
            const foreignDocs2 = [{_id: 0, c: 0}, {_id: 2, c: 2}, {_id: 3}];
            assert.commandWorked(localColl.insert(localDocs));
            assert.commandWorked(foreignColl1.insert(foreignDocs1));
            assert.commandWorked(foreignColl2.insert(foreignDocs2));
            createIndex(currentJoinAlgorithm, foreignColl1, "b");
            createIndex(currentJoinAlgorithm, foreignColl2, "c");

            const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};
            const results = localColl
                .aggregate(
                    [
                        {
                            $lookup: {
                                from: foreignColl1.getName(),
                                localField: "a",
                                foreignField: "b",
                                as: "foreign1",
                            },
                        },
                        {
                            $unwind: {
                                path: "$foreign1",
                                preserveNullAndEmptyArrays: preserveNullAndEmptyArrays,
                                includeArrayIndex: "index1",
                            },
                        },
                        {
                            $lookup: {
                                from: foreignColl2.getName(),
                                localField: "foreign1.b",
                                foreignField: "c",
                                as: "foreign2",
                            },
                        },
                        {
                            $unwind: {
                                path: "$foreign2",
                                preserveNullAndEmptyArrays: preserveNullAndEmptyArrays,
                                includeArrayIndex: "index2",
                            },
                        },
                    ],
                    aggOptions,
                )
                .toArray();

            assertArrayEq({
                actual: results,
                expected: matchedDocs,
            });
        }
        testRunner(
            [
                {
                    "_id": 0,
                    "a": 0,
                    "foreign1": {
                        "_id": 0,
                        "b": 0,
                    },
                    "index1": 0,
                    "foreign2": {
                        "_id": 0,
                        "c": 0,
                    },
                    "index2": 0,
                },
            ],
            false /*preserveNullAndEmptyArrays*/,
        );
        testRunner(
            [
                {
                    "_id": 0,
                    "a": 0,
                    "foreign1": {
                        "_id": 0,
                        "b": 0,
                    },
                    "index1": 0,
                    "foreign2": {
                        "_id": 0,
                        "c": 0,
                    },
                    "index2": 0,
                },
                {
                    "_id": 1,
                    "a": 1,
                    "foreign1": {
                        "_id": 1,
                        "b": 1,
                    },
                    "index1": 0,
                    "index2": null,
                },
                {
                    "_id": 2,
                    "a": 42,
                    "index1": null,
                    foreign2: {
                        _id: 3,
                    },
                    "index2": 0,
                },
            ],
            true /*preserveNullAndEmptyArrays*/,
        );
    }

    function testSelfLookup() {
        function testRunner(matchedDocs, preserveNullAndEmptyArrays) {
            localColl.drop();
            const localDocs = [
                {_id: 0, a: 2},
                {_id: 1, a: 0},
                {_id: 2, a: 1},
                {_id: 3, a: 42},
            ];
            assert.commandWorked(localColl.insert(localDocs));
            createIndex(currentJoinAlgorithm, localColl, "a");

            const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};
            const results = localColl
                .aggregate(
                    [
                        {
                            $lookup: {
                                from: localColl.getName(),
                                localField: "_id",
                                foreignField: "a",
                                as: "matched",
                            },
                        },
                        {
                            $unwind: {
                                path: "$matched",
                                preserveNullAndEmptyArrays: preserveNullAndEmptyArrays,
                                includeArrayIndex: "index",
                            },
                        },
                    ],
                    aggOptions,
                )
                .toArray();

            assertArrayEq({
                actual: results,
                expected: matchedDocs,
            });
        }

        testRunner(
            [
                {_id: 0, a: 2, matched: {_id: 1, a: 0}, index: 0},
                {_id: 1, a: 0, matched: {_id: 2, a: 1}, index: 0},
                {_id: 2, a: 1, matched: {_id: 0, a: 2}, index: 0},
            ],
            false /*preserveNullAndEmptyArrays*/,
        );
        testRunner(
            [
                {_id: 0, a: 2, matched: {_id: 1, a: 0}, index: 0},
                {_id: 1, a: 0, matched: {_id: 2, a: 1}, index: 0},
                {_id: 2, a: 1, matched: {_id: 0, a: 2}, index: 0},
                {_id: 3, a: 42, index: null},
            ],
            true /*preserveNullAndEmptyArrays*/,
        );
    }

    function testNonExistentForeignCollection() {
        function testRunner(matchedDocs, preserveNullAndEmptyArrays) {
            localColl.drop();
            const localDoc = {_id: 0, a: 1};
            assert.commandWorked(localColl.insert(localDoc));

            foreignColl.drop();

            const aggOptions = {allowDiskUse: currentJoinAlgorithm == JoinAlgorithm.HJ};
            const results = localColl
                .aggregate(
                    [
                        {
                            $lookup: {
                                from: foreignColl.getName(),
                                localField: "a",
                                foreignField: "b",
                                as: "matched",
                            },
                        },
                        {
                            $unwind: {
                                path: "$matched",
                                preserveNullAndEmptyArrays: preserveNullAndEmptyArrays,
                                includeArrayIndex: "index",
                            },
                        },
                    ],
                    aggOptions,
                )
                .toArray();

            assertArrayEq({
                actual: results,
                expected: matchedDocs,
            });
        }

        testRunner([], false /*preserveNullAndEmptyArrays*/);
        testRunner([{_id: 0, a: 1, index: null}], true /*preserveNullAndEmptyArrays*/);
    }

    // Hashed indexes do not currently support array values, so skip the test.
    if (currentJoinAlgorithm != JoinAlgorithm.INLJ_Hashed) {
        testMatchingNullAndVariousDataTypes();
    }
    testMissingLocalField();
    testMultiJoinOnLocalCollection();
    testMultiJoinForeignOnForeign();
    testSelfLookup();
    testNonExistentForeignCollection();
}

runTests({
    localColl: db.local,
    foreignColl: db.foreign,
    currentJoinAlgorithm: JoinAlgorithm.HJ,
});

runTests({
    localColl: db.local,
    foreignColl: db.foreign,
    currentJoinAlgorithm: JoinAlgorithm.NLJ,
});

runTests({
    localColl: db.local,
    foreignColl: db.foreign,
    currentJoinAlgorithm: JoinAlgorithm.INLJ_Asc,
});

runTests({
    localColl: db.local,
    foreignColl: db.foreign,
    currentJoinAlgorithm: JoinAlgorithm.INLJ_Dec,
});

runTests({
    localColl: db.local,
    foreignColl: db.foreign,
    currentJoinAlgorithm: JoinAlgorithm.INLJ_Hashed,
});
