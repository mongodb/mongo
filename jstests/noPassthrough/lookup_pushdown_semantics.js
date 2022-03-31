/**
 * Tests correctness of output of pushing $lookup into the find layer.
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled()'.
load("jstests/aggregation/extras/utils.js");

// Standalone cases.
const conn = MongoRunner.runMongod({setParameter: "featureFlagSBELookupPushdown=true"});
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("lookup_pushdown");
if (!checkSBEEnabled(db, ["featureFlagSBELookupPushdown"])) {
    jsTestLog("Skipping test because either the sbe lookup pushdown feature flag is disabled or" +
              " sbe itself is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const localColl = db["lookup_pushdown_local"];
const foreignColl = db["lookup_pushdown_foreign"];

/**
 * Executes $lookup with exactly one record in the foreign collection, so we don't need to check the
 * content of the "as" field but only that it's not empty for local records with ids in
 * 'idsExpectToMatch'.
 */
function runTest_SingleForeignRecord({
    testDescription,
    localRecords,
    localField,
    foreignRecord,
    foreignField,
    foreignIndex,
    idsExpectedToMatch
}) {
    assert('object' === typeof (foreignRecord) && !Array.isArray(foreignRecord),
           "foreignRecord should be a single document");

    localColl.drop();
    assert.commandWorked(localColl.insert(localRecords));

    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecord));

    if (foreignIndex) {
        assert.commandWorked(foreignColl.createIndex(foreignIndex));
        testDescription += ` (foreign index ${tojson(foreignIndex)})`;
    }

    const results = localColl.aggregate([{
        $lookup: {
            from: foreignColl.getName(),
            localField: localField,
            foreignField: foreignField,
            as: "matched"
        }
    }]).toArray();

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
        extraErrorMsg: " **TEST** " + testDescription
    });
}

/**
 * Executes $lookup with exactly one record in the local collection and checks that the "as" field
 * for it contains documents with ids from `idsExpectedToMatch`.
 */
function runTest_SingleLocalRecord({
    testDescription,
    localRecord,
    localField,
    foreignRecords,
    foreignField,
    foreignIndex,
    idsExpectedToMatch
}) {
    assert('object' === typeof (localRecord) && !Array.isArray(localRecord),
           "localRecord should be a single document");

    localColl.drop();
    assert.commandWorked(localColl.insert(localRecord));

    foreignColl.drop();
    assert.commandWorked(foreignColl.insert(foreignRecords));

    if (foreignIndex) {
        assert.commandWorked(foreignColl.createIndex(foreignIndex));
        testDescription += ` (foreign index ${tojson(foreignIndex)})`;
    }

    const results = localColl.aggregate([{
        $lookup: {
            from: foreignColl.getName(),
            localField: localField,
            foreignField: foreignField,
            as: "matched"
        }
    }]).toArray();
    assert.eq(1, results.length);

    // Extract matched foreign ids from the "matched" field.
    const matchedIds = results[0].matched.map(x => x._id);

    // Order of the elements within the arrays is not significant for 'assertArrayEq'.
    assertArrayEq({
        actual: matchedIds,
        expected: idsExpectedToMatch,
        extraErrorMsg: " **TEST** " + testDescription
    });
}

/**
 * Executes $lookup with non existent foreign collection and checks that the "as" field for it
 * contains empty arrays.
 */
(
    function runTest_NonExistentForeignCollection() {
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

function testMatchingTopLevelFieldToNonArray(indexType) {
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
        runTest_SingleForeignRecord({
            testDescription:
                "Top-level field in local and top-level scalar in foreign with index on foreign field and produces single match",
            localRecords: docs,
            localField: "a",
            foreignRecord: {b: doc.a},
            foreignField: "b",
            foreignIndex: {b: indexType},
            idsExpectedToMatch: [doc._id]
        });
        runTest_SingleLocalRecord({
            testDescription:
                "Top-level scalar in local and top-level field in foreign with index on foreign field and produces single match",
            localRecord: {b: doc.a},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            foreignIndex: {a: indexType},
            idsExpectedToMatch: [doc._id]
        });
    });

    runTest_SingleForeignRecord({
        testDescription:
            "Top-level field in local and top-level scalar in foreign with index on foreign field and produces no match",
        localRecords: docs,
        localField: "a",
        foreignRecord: {b: 'xxx'},
        foreignField: "b",
        foreignIndex: {b: indexType},
        idsExpectedToMatch: []
    });
    runTest_SingleLocalRecord({
        testDescription:
            "Top-level scalar in local and top-level field in foreign with index on foreign field and produces no match",
        localRecord: {b: 'xxx'},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        foreignIndex: {a: indexType},
        idsExpectedToMatch: []
    });
}

testMatchingTopLevelFieldToNonArray(1 /* indexType */);
testMatchingTopLevelFieldToNonArray(-1 /* indexType */);
testMatchingTopLevelFieldToNonArray("hashed" /* indexType */);

function testMatchingTopLevelFieldToNullAndUndefined(indexType) {
    const foreignRecords = [
        {_id: 0, a: null},
        {_id: 1, a: undefined},
    ];
    // We do not currently support hashed indexes on the collections with arrays.
    if (indexType != "hashed") {
        foreignRecords.push({_id: 2, a: []}, {_id: 3, a: [[]]});
    }

    runTest_SingleLocalRecord({
        testDescription: "Null should match only to null",
        localRecord: {b: null},
        localField: "b",
        foreignRecords,
        foreignField: "a",
        foreignIndex: {a: indexType},
        idsExpectedToMatch: [0]
    });
}

testMatchingTopLevelFieldToNullAndUndefined(1 /* indexType */);
testMatchingTopLevelFieldToNullAndUndefined(-1 /* indexType */);
testMatchingTopLevelFieldToNullAndUndefined("hashed" /* indexType */);

function testMatchingTopLevelFieldToArrays(indexType) {
    runTest_SingleLocalRecord({
        testDescription: "Scalar should match arrays containing that value",
        localRecord: {b: 1},
        localField: "b",
        foreignRecords: [
            {_id: 0, a: 1},
            {_id: 1, a: [1]},
            {_id: 2, a: [1, 2, 3]},
            {_id: 3, a: [3, 2, 1]},
            {_id: 4, a: [4, 5, 6]},
            {_id: 5, a: []},
        ],
        foreignField: "a",
        foreignIndex: {a: indexType},
        idsExpectedToMatch: [0, 1, 2, 3]
    });

    runTest_SingleLocalRecord({
        testDescription: "Empty array should only match to empty array",
        localRecord: {b: [[]]},
        localField: "b",
        foreignRecords: [
            {_id: 0, a: null},
            {_id: 1, a: undefined},
            {_id: 2, a: []},
            {_id: 3, a: [[]]},
            {_id: 4, a: [null]},
            {_id: 5, a: [undefined]},
            {_id: 6, a: [1]},
            {_id: 7, a: [1, 2, 3]},
        ],
        foreignField: "a",
        foreignIndex: {a: indexType},
        idsExpectedToMatch: [2, 3]
    });

    runTest_SingleLocalRecord({
        testDescription: "Single element arrays should match only single-element arrays",
        localRecord: {b: [[1]]},
        localField: "b",
        foreignRecords: [
            {_id: 0, a: 1},
            {_id: 1, a: [1]},
            {_id: 2, a: [1, 2, 3]},
            {_id: 3, a: [3, 2, 1]},
            {_id: 4, a: [4, 5, 6]},
            {_id: 5, a: []},
        ],
        foreignField: "a",
        foreignIndex: {a: indexType},
        idsExpectedToMatch: [1]
    });

    runTest_SingleLocalRecord({
        testDescription: "Arrays with multiple elements should only match itself",
        localRecord: {b: [[1, 2, 3]]},
        localField: "b",
        foreignRecords: [
            {_id: 0, a: 1},
            {_id: 1, a: [1]},
            {_id: 2, a: [1, 2, 3]},
            {_id: 3, a: [3, 2, 1]},
            {_id: 4, a: [4, 5, 6]},
            {_id: 5, a: []},
        ],
        foreignField: "a",
        foreignIndex: {a: indexType},
        idsExpectedToMatch: [2]
    });

    runTest_SingleLocalRecord({
        testDescription: "Array queries must work on hashed indexes",
        localRecord: {b: [[1, 2, 3]]},
        localField: "b",
        foreignRecords: [
            {_id: 0, a: 1},
        ],
        foreignField: "a",
        foreignIndex: {a: "hashed"},
        idsExpectedToMatch: []
    });
}

testMatchingTopLevelFieldToArrays(1 /* indexType */);
testMatchingTopLevelFieldToArrays(-1 /* indexType */);

function testMatchingWithNestedPaths(indexType) {
    const foreignRecords = [
        {_id: 0, a: {b: {c: 1}}},
        {_id: 1, a: {no_b: 1}},
        {_id: 2, a: {b: {no_c: 1}}},
    ];
    const idsExpectedToMatch = [0];

    // We do not currently support hashed indexes on the collections with arrays.
    if (indexType != "hashed") {
        foreignRecords.push({_id: 3, a: {b: {c: [1]}}},
                            {_id: 4, a: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {c: 4}]}]});
        idsExpectedToMatch.push(3, 4);
    }

    runTest_SingleLocalRecord({
        testDescription: "Index join with nested path in foreign field",
        localRecord: {b: 1},
        localField: "b",
        foreignRecords,
        foreignField: "a.b.c",
        foreignIndex: {"a.b.c": indexType},
        idsExpectedToMatch,
    });

    runTest_SingleForeignRecord({
        testDescription: "Index join with nested path in local field",
        localRecords: [
            {_id: 0, a: {b: {c: 1}}},
            {_id: 1, a: {b: {c: [1]}}},
            {_id: 2, a: [{b: [{c: 1}, {c: 2}]}, {b: [{c: 3}, {c: 4}]}]},
            {_id: 3, a: {no_b: 1}},
            {_id: 4, a: {b: {no_c: 1}}},
        ],
        localField: "a.b.c",
        foreignRecord: {b: 1},
        foreignField: "b",
        foreignIndex: {b: indexType},
        idsExpectedToMatch: [0, 1, 2]
    });
}

testMatchingWithNestedPaths(1 /* indexType */);
testMatchingWithNestedPaths(-1 /* indexType */);
testMatchingWithNestedPaths("hashed" /* indexType */);

MongoRunner.stopMongod(conn);
}());
