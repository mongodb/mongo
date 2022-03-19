/**
 * Tests correctness of output of pushing $lookup into the find layer.
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled()'.
load("jstests/aggregation/extras/utils.js");

// Standalone cases.
const conn = MongoRunner.runMongod({
    setParameter: {featureFlagSBELookupPushdown: true, featureFlagSBELookupPushdownIndexJoin: true}
});
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

(function testMatchingTopLevelFieldToScalar() {
    const docs = [
        {_id: 0, a: NumberInt(0)},
        {_id: 1, a: 3.14},
        {_id: 2, a: NumberDecimal(3.14)},
        {_id: 3, a: "abc"},
    ];

    docs.forEach(doc => {
        runTest_SingleForeignRecord({
            testDescription:
                "Top-level field in local and top-level scalar in foreign with index on foreign field and produces single match",
            localRecords: docs,
            localField: "a",
            foreignRecord: {b: doc.a},
            foreignField: "b",
            foreignIndex: {b: 1},
            idsExpectedToMatch: [doc._id]
        });
        runTest_SingleLocalRecord({
            testDescription:
                "Top-level scalar in local and top-level field in foreign with index on foreign field and produces single match",
            localRecord: {b: doc.a},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            foreignIndex: {a: 1},
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
        foreignIndex: {b: 1},
        idsExpectedToMatch: []
    });
    runTest_SingleLocalRecord({
        testDescription:
            "Top-level scalar in local and top-level field in foreign with index on foreign field and produces no match",
        localRecord: {b: 'xxx'},
        localField: "b",
        foreignRecords: docs,
        foreignField: "a",
        foreignIndex: {a: 1},
        idsExpectedToMatch: []
    });
})();

MongoRunner.stopMongod(conn);
}());
