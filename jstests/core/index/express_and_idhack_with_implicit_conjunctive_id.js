/**
 * Tests the express and idhack code path for query predicates with implicit conjunctive _id
 * @tags: [
 *   requires_fcv_80,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   requires_getmore,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {isExpress, isIdhackOrExpress} from "jstests/libs/analyze_plan.js";
import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const collName = 'express_and_idhack_with_implicit_conjunctive_id';
const coll = db.getCollection(collName);
const docs = [
    {_id: 0, a: 0},
    {_id: "str", a: 1},
];

function runExpressReadTest({command, expectedDocs, usesExpress, usesIdHack}) {
    // Reset the collection docs.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(docs));

    // Run the command to make sure it succeeds.
    let res = assert.commandWorked(db.runCommand(command));
    if (command.count) {
        assert.eq(res.n, expectedDocs, "Expected count " + expectedDocs + " but found " + res.n);
    } else if (command.distinct) {
        assertArrayEq({
            actual: res.values,
            expected: expectedDocs,
            extraErrorMsg: "Result set comparison failed for command: " + tojson(command)
        });
    } else {
        assertArrayEq({
            actual: res.cursor.firstBatch,
            expected: expectedDocs,
            extraErrorMsg: "Result set comparison failed for command: " + tojson(command)
        });
    }

    const explain =
        assert.commandWorked(db.runCommand({explain: command, verbosity: "executionStats"}));

    assert.eq(
        usesExpress,
        isExpress(db, explain),
        "Expected the query to " + (usesExpress ? "" : "not ") + "use express: " + tojson(explain));

    const isIdhack = isIdhackOrExpress(db, explain) && !isExpress(db, explain);

    assert.eq(
        usesIdHack,
        isIdhack,
        "Expected the query to " + (usesIdHack ? "" : "not ") + "use idhack: " + tojson(explain));
}

function runExpressWriteTest({command, expectedDocs, usesExpress, usesIdHack}) {
    // Reset the collection docs.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(docs));

    // Run the command to make sure it succeeds.
    assert.commandWorked(db.runCommand(command));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: expectedDocs,
        extraErrorMsg: "Result set comparison failed for command: " + tojson(command)
    });

    // Reset the collection docs then run explain.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(docs));
    const explain =
        assert.commandWorked(db.runCommand({explain: command, verbosity: "executionStats"}));

    assert.eq(
        usesExpress,
        isExpress(db, explain),
        "Expected the query to " + (usesExpress ? "" : "not ") + "use express: " + tojson(explain));

    const isIdhack = isIdhackOrExpress(db, explain) && !isExpress(db, explain);

    assert.eq(
        usesIdHack,
        isIdhack,
        "Expected the query to " + (usesIdHack ? "" : "not ") + "use idhack: " + tojson(explain));
}

coll.drop();
coll.createIndex({a: 1}, {unique: true});

const isShardedColl = FixtureHelpers.isSharded(coll);
const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());

// Find
runExpressReadTest({
    command: {find: collName, filter: _buildBsonObj("_id", 0, "_id", 0)},
    usesExpress: !collectionIsClustered,
    usesIdHack: collectionIsClustered,
    expectedDocs: [docs[0]]
});

runExpressReadTest({
    command: {find: collName, filter: _buildBsonObj("_id", 0, "_id", 1)},
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: []
});

runExpressReadTest({
    command: {find: collName, filter: _buildBsonObj("a", 0, "a", 0)},
    usesExpress: !isShardedColl && !collectionIsClustered,
    usesIdHack: false,
    expectedDocs: [docs[0]]
});

runExpressReadTest({
    command: {find: collName, filter: _buildBsonObj("a", 0, "a", 1)},
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: []
});

// Aggregation
runExpressReadTest({
    command:
        {aggregate: collName, pipeline: [{$match: _buildBsonObj("_id", 0, "_id", 0)}], cursor: {}},
    usesExpress: !collectionIsClustered,
    usesIdHack: collectionIsClustered,
    expectedDocs: [docs[0]]
});

runExpressReadTest({
    command:
        {aggregate: collName, pipeline: [{$match: _buildBsonObj("_id", 0, "_id", 1)}], cursor: {}},
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: []
});

runExpressReadTest({
    command: {aggregate: collName, pipeline: [{$match: _buildBsonObj("a", 0, "a", 0)}], cursor: {}},
    usesExpress: !isShardedColl && !collectionIsClustered,
    usesIdHack: false,
    expectedDocs: [docs[0]]
});

runExpressReadTest({
    command: {aggregate: collName, pipeline: [{$match: _buildBsonObj("a", 0, "a", 1)}], cursor: {}},
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: []
});

// Count
runExpressReadTest({
    command: {count: collName, query: _buildBsonObj("_id", 0, "_id", 0)},
    usesExpress: false,
    usesIdHack: collectionIsClustered,
    expectedDocs: 1
});

runExpressReadTest({
    command: {count: collName, query: _buildBsonObj("_id", 0, "_id", 1)},
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: 0
});

// Distinct
runExpressReadTest({
    command: {distinct: collName, key: "a", query: _buildBsonObj("_id", 0, "_id", 0)},
    usesExpress: !collectionIsClustered,
    usesIdHack: collectionIsClustered,
    expectedDocs: [docs[0].a]
});

runExpressReadTest({
    command: {distinct: collName, key: "a", query: _buildBsonObj("_id", 0, "_id", 1)},
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: []
});

// Delete
runExpressWriteTest({
    command: {delete: collName, deletes: [{q: _buildBsonObj("_id", 0, "_id", 0), limit: 0}]},
    usesExpress: false,
    usesIdHack: collectionIsClustered,
    expectedDocs: [docs[1]]
});

runExpressWriteTest({
    command: {delete: collName, deletes: [{q: _buildBsonObj("_id", 0, "_id", 1), limit: 0}]},
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: [docs[0], docs[1]]
});

// FindAndModify
runExpressWriteTest({
    command: {
        findAndModify: collName,
        query: _buildBsonObj("_id", 0, "_id", 0),
        update: [{$set: {c: 1}}]
    },
    usesExpress: false,
    usesIdHack: collectionIsClustered,
    expectedDocs: [{_id: 0, a: 0, c: 1}, docs[1]]
});

runExpressWriteTest({
    command: {
        findAndModify: collName,
        query: _buildBsonObj("_id", 0, "_id", 1),
        update: [{$set: {c: 1}}]
    },
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: [docs[0], docs[1]]
});

// Update
runExpressWriteTest({
    command:
        {update: collName, updates: [{q: _buildBsonObj("_id", 0, "_id", 0), u: {$set: {c: 1}}}]},
    usesExpress: false,
    usesIdHack: collectionIsClustered,
    expectedDocs: [{_id: 0, a: 0, c: 1}, docs[1]]
});

runExpressWriteTest({
    command: {
        update: collName,
        updates: [{q: _buildBsonObj("_id", 0, "_id", 1), u: {$set: {c: 1}}, multi: true}]
    },
    usesExpress: false,
    usesIdHack: false,
    expectedDocs: [docs[0], docs[1]]
});
