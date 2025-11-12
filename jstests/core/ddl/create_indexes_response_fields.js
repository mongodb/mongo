/**
 * Tests the fields in the response of the createIndexes command.
 *
 * @tags: [
 *  # The createIndexes response fields are not reliable on stepdowns due to implicit retries.
 *  # Therefore, the numIndexesBefore may be equal to numIndexesAfter if the index was created
 *  # before the migration.
 *  does_not_support_stepdowns,
 *  # Migrations may interrupt createIndexes operations, forcing a retry. Therefore, the
 *  # numIndexesBefore may be equal to numIndexesAfter if the index was created before the
 *  # migration.
 *  assumes_balancer_off,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const dbTest = db.getSiblingDB(jsTestName());
dbTest.dropDatabase();

function checkResponse(res, coll, checkFunc) {
    assert(checkFunc, "checkFunc must be provided to checkResponse");

    if (!FixtureHelpers.isMongos(dbTest)) {
        checkFunc(res);
        return;
    }

    // On sharded clusters, check the 'raw' field for each shard.
    assert(res.hasOwnProperty("raw"), "Expected 'raw' field in createIndexes response: " + tojson(res));

    for (const shardField in res.raw) {
        checkFunc(res.raw[shardField]);
    }

    if (!FixtureHelpers.isSharded(coll)) {
        // If the collection is not sharded, also check the top-level response.
        checkFunc(res);
    }
}

// Database does not exist
const collDbNotExist = dbTest.create_indexes_no_db;
let res = assert.commandWorked(collDbNotExist.runCommand("createIndexes", {indexes: [{key: {x: 1}, name: "x_1"}]}));
checkResponse(res, collDbNotExist, (res) => {
    assert.eq(res.numIndexesAfter, res.numIndexesBefore + 1);
    assert.isnull(
        res.note,
        "createIndexes.note should not be present in results when adding a new index: " + tojson(res),
    );
});

// Collection does not exist, but database does
const t = dbTest.create_indexes;
res = assert.commandWorked(t.runCommand("createIndexes", {indexes: [{key: {x: 1}, name: "x_1"}]}));
checkResponse(res, t, (res) => {
    assert.eq(res.numIndexesAfter, res.numIndexesBefore + 1);
    assert.isnull(
        res.note,
        "createIndexes.note should not be present in results when adding a new index: " + tojson(res),
    );
});

// Both database and collection exist
res = assert.commandWorked(t.runCommand("createIndexes", {indexes: [{key: {x: 1}, name: "x_1"}]}));
checkResponse(res, t, (res) => {
    assert.eq(
        res.numIndexesBefore,
        res.numIndexesAfter,
        "numIndexesAfter missing from createIndexes result when adding a duplicate index: " + tojson(res),
    );
    assert(res.note, "createIndexes.note should be present in results when adding a duplicate index: " + tojson(res));
});

res = t.runCommand("createIndexes", {
    indexes: [
        {key: {"x": 1}, name: "x_1"},
        {key: {"y": 1}, name: "y_1"},
    ],
});
checkResponse(res, t, (res) => {
    assert.eq(res.numIndexesAfter, res.numIndexesBefore + 1);
});

res = assert.commandWorked(
    t.runCommand("createIndexes", {
        indexes: [
            {key: {a: 1}, name: "a_1"},
            {key: {b: 1}, name: "b_1"},
        ],
    }),
);
checkResponse(res, t, (res) => {
    assert.eq(res.numIndexesAfter, res.numIndexesBefore + 2);
    assert.isnull(
        res.note,
        "createIndexes.note should not be present in results when adding new indexes: " + tojson(res),
    );
});

res = assert.commandWorked(
    t.runCommand("createIndexes", {
        indexes: [
            {key: {a: 1}, name: "a_1"},
            {key: {b: 1}, name: "b_1"},
        ],
    }),
);

checkResponse(res, t, (res) => {
    assert.eq(
        res.numIndexesBefore,
        res.numIndexesAfter,
        "numIndexesAfter missing from createIndexes result when adding duplicate indexes: " + tojson(res),
    );
    assert(res.note, "createIndexes.note should be present in results when adding a duplicate index: " + tojson(res));
});
