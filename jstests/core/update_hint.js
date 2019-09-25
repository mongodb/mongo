/**
 * Tests passing hint to the update command:
 *   - A bad argument to the hint option should raise an error.
 *   - The hint option should support both the name of the index, and the object spec of the
 *     index.
 *
 * @tags: [assumes_unsharded_collection, requires_non_retryable_writes]
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

const coll = db.jstests_update_hint;
let updateCmd = {update: coll.getName(), updates: [{q: {x: 1}, u: {$set: {y: 1}}, hint: {x: 1}}]};

function normalIndexTest() {
    // Hint using a key pattern.
    coll.drop();
    assert.commandWorked(coll.insert({x: 1, y: 1}));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: -1}));

    // Hint using index key pattern.
    assertCommandUsesIndex(updateCmd, {x: 1});

    // Hint using an index name.
    updateCmd = {update: coll.getName(), updates: [{q: {x: 1}, u: {$set: {y: 1}}, hint: 'y_-1'}]};
    assertCommandUsesIndex(updateCmd, {y: -1});

    // Passing a hint should not use the idhack fast-path.
    updateCmd = {update: coll.getName(), updates: [{q: {_id: 1}, u: {$set: {y: 1}}, hint: 'y_-1'}]};
    assertCommandUsesIndex(updateCmd, {y: -1});
}

function sparseIndexTest() {
    // Create a sparse index with 2 documents.
    coll.drop();
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.insert({x: 1, s: 0}));
    assert.commandWorked(coll.insert({x: 1, s: 0}));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({s: 1}, {sparse: true}));

    // Hint should be respected, even on incomplete indexes.
    updateCmd = {update: coll.getName(), updates: [{q: {_id: 1}, u: {$set: {y: 1}}, hint: {s: 1}}]};
    assertCommandUsesIndex(updateCmd, {s: 1});

    // Update hinting a sparse index updates only the document in the sparse index.
    updateCmd = {
        update: coll.getName(),
        updates: [{q: {}, u: {$set: {s: 1}}, hint: {s: 1}, multi: true}]
    };
    assert.commandWorked(coll.runCommand(updateCmd));
    assert.eq(2, coll.count({s: 1}));

    // Update hinting a sparse index with upsert option can result in an insert even if the
    // correct behaviour would be to update an existing document.
    assert.commandWorked(coll.insert({x: 2}));
    updateCmd = {
        update: coll.getName(),
        updates: [{q: {x: 2}, u: {$set: {x: 1}}, hint: {s: 1}, upsert: true}]
    };
    let res = assert.commandWorked(coll.runCommand(updateCmd));
    assert.eq(res.upserted.length, 1);
}

function shellHelpersTest() {
    coll.drop();
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.insert({x: 1, s: 0}));
    assert.commandWorked(coll.insert({x: 1, s: 0}));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({s: 1}, {sparse: true}));

    // Test shell helpers using a hinted sparse index should only update documents that exist in
    // the sparse index.
    let res = coll.update({x: 1}, {$set: {y: 2}}, {hint: {s: 1}, multi: true});
    assert.eq(res.nMatched, 2);

    // Insert document that will not be in the sparse index. Update hinting sparse index should
    // result in upsert.
    assert.commandWorked(coll.insert({x: 2}));
    res = coll.updateOne({x: 2}, {$set: {y: 2}}, {hint: {s: 1}, upsert: true});
    assert(res.upsertedId);
    res = coll.updateMany({x: 1}, {$set: {y: 2}}, {hint: {s: 1}});
    assert.eq(res.matchedCount, 2);

    // Test bulk writes.
    let bulk = coll.initializeUnorderedBulkOp();
    bulk.find({x: 1}).hint({s: 1}).update({$set: {y: 1}});
    res = bulk.execute();
    assert.eq(res.nMatched, 2);
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({x: 2}).hint({s: 1}).upsert().updateOne({$set: {y: 1}});
    res = bulk.execute();
    assert.eq(res.nUpserted, 1);
    bulk = coll.initializeUnorderedBulkOp();
    bulk.find({x: 2}).hint({s: 1}).upsert().replaceOne({$set: {y: 1}});
    res = bulk.execute();
    assert.eq(res.nUpserted, 1);

    res = coll.bulkWrite([{
        updateOne: {
            filter: {x: 2},
            update: {$set: {y: 2}},
            hint: {s: 1},
            upsert: true,
        }
    }]);
    assert.eq(res.upsertedCount, 1);

    res = coll.bulkWrite([{
        updateMany: {
            filter: {x: 1},
            update: {$set: {y: 2}},
            hint: {s: 1},
        }
    }]);
    assert.eq(res.matchedCount, 2);

    res = coll.bulkWrite([{
        replaceOne: {
            filter: {x: 2},
            replacement: {x: 2, y: 3},
            hint: {s: 1},
            upsert: true,
        }
    }]);
    assert.eq(res.upsertedCount, 1);
}

function failedHintTest() {
    coll.drop();
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.createIndex({x: 1}));

    // Command should fail with incorrectly formatted hints.
    updateCmd = {update: coll.getName(), updates: [{q: {_id: 1}, u: {$set: {y: 1}}, hint: 1}]};
    assert.commandFailedWithCode(coll.runCommand(updateCmd), ErrorCodes.FailedToParse);
    updateCmd = {update: coll.getName(), updates: [{q: {_id: 1}, u: {$set: {y: 1}}, hint: true}]};
    assert.commandFailedWithCode(coll.runCommand(updateCmd), ErrorCodes.FailedToParse);

    // Command should fail with hints to non-existent indexes.
    updateCmd = {
        update: coll.getName(),
        updates: [{q: {_id: 1}, u: {$set: {y: 1}}, hint: {badHint: 1}}]
    };
    assert.commandFailedWithCode(coll.runCommand(updateCmd), ErrorCodes.BadValue);
}

normalIndexTest();
sparseIndexTest();
if (coll.getMongo().writeMode() === "commands") {
    shellHelpersTest();
}
failedHintTest();
})();
