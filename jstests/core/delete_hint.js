/**
 * Tests passing hint to the delete command:
 *   - A bad argument to the hint option should raise an error.
 *   - The hint option should support both the name of the index, and the object spec of the
 *     index.
 *
 * @tags: [assumes_unsharded_collection, requires_non_retryable_writes, requires_fcv_44]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

function assertCommandUsesIndex(command, expectedHintKeyPattern) {
    const out = assert.commandWorked(coll.runCommand({explain: command}));
    const planStage = getPlanStage(out, "IXSCAN");
    assert.neq(null, planStage);
    assert.eq(planStage.keyPattern, expectedHintKeyPattern, tojson(planStage));
}

const coll = db.jstests_delete_hint;

function normalIndexTest() {
    // Hint using a key pattern.
    coll.drop();
    assert.commandWorked(coll.insert({x: 1, y: 1}));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: -1}));

    // Hint using index key pattern.
    let deleteCmd = {delete: coll.getName(), deletes: [{q: {x: 1}, limit: 1, hint: {x: 1}}]};
    assertCommandUsesIndex(deleteCmd, {x: 1});

    // Hint using an index name.
    deleteCmd = {delete: coll.getName(), deletes: [{q: {x: 1}, limit: 1, hint: 'y_-1'}]};
    assertCommandUsesIndex(deleteCmd, {y: -1});

    // Passing a hint should not use the idhack fast-path.
    deleteCmd = {delete: coll.getName(), deletes: [{q: {_id: 1}, limit: 1, hint: 'y_-1'}]};
    assertCommandUsesIndex(deleteCmd, {y: -1});
}

function sparseIndexTest() {
    // Create a sparse index with 2 documents.
    coll.drop();
    assert.commandWorked(coll.insert([{x: 1}, {x: 1}, {x: 1, s: 0}, {x: 1, s: 0}]));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({s: 1}, {sparse: true}));

    // Hint should be respected, even on incomplete indexes.
    let deleteCmd = {delete: coll.getName(), deletes: [{q: {_id: 1}, limit: 1, hint: {s: 1}}]};
    assertCommandUsesIndex(deleteCmd, {s: 1});

    // Delete hinting a sparse index deletes only the document in the sparse index.
    deleteCmd = {delete: coll.getName(), deletes: [{q: {}, limit: 0, hint: {s: 1}}]};
    let res = assert.commandWorked(coll.runCommand(deleteCmd));
    assert.eq(2, res.n);
}

function shellHelpersTest() {
    coll.drop();
    assert.commandWorked(coll.insert([{x: 1}, {x: 1, s: 0}, {x: 1, s: 0}]));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({s: 1}, {sparse: true}));

    // Test shell helpers using a hinted sparse index should only delete documents that exist in
    // the sparse index.
    let res = coll.deleteOne({x: 1}, {hint: {s: 1}});
    assert.eq(res.deletedCount, 1);

    // Test bulk writes.
    let bulk = coll.initializeUnorderedBulkOp();
    bulk.find({x: 1}).hint({s: 1}).remove();
    res = bulk.execute();
    assert.eq(res.nRemoved, 2);
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({x: 2}).hint({s: 1}).removeOne();
    res = bulk.execute();
    assert.eq(res.nRemoved, 0);

    assert.commandWorked(coll.insert([{x: 0}, {x: 1, s: 0}, {x: 1, s: 0}, {x: 1, s: 0}]));
    res = coll.bulkWrite([{deleteOne: {filter: {x: 1}, hint: {s: 1}}}]);
    assert.eq(res.deletedCount, 1);

    res = coll.bulkWrite([{
        deleteMany: {
            filter: {x: 1},
            hint: {s: 1},
        }
    }]);
    assert.eq(res.deletedCount, 2);
}

function failedHintTest() {
    coll.drop();
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.createIndex({x: 1}));

    // Command should fail with incorrectly formatted hints.
    let deleteCmd = {delete: coll.getName(), deletes: [{q: {_id: 1}, limit: 1, hint: 1}]};
    assert.commandFailedWithCode(coll.runCommand(deleteCmd), ErrorCodes.FailedToParse);
    deleteCmd = {delete: coll.getName(), deletes: [{q: {_id: 1}, limit: 1, hint: true}]};
    assert.commandFailedWithCode(coll.runCommand(deleteCmd), ErrorCodes.FailedToParse);

    // Command should fail with hints to non-existent indexes.
    deleteCmd = {delete: coll.getName(), deletes: [{q: {_id: 1}, limit: 1, hint: {badHint: 1}}]};
    assert.commandFailedWithCode(coll.runCommand(deleteCmd), ErrorCodes.BadValue);
}

normalIndexTest();
sparseIndexTest();
if (coll.getMongo().writeMode() === "commands") {
    shellHelpersTest();
}
failedHintTest();
})();
