/**
 * Tests passing hint to the findAndModify command:
 *   - A bad argument to the hint option should raise an error.
 *   - The hint option should support both the name of the index, and an index spec object.
 *
 * @tags: [assumes_unsharded_collection, requires_non_retryable_writes, requires_find_command,
 * requires_fcv_44]
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

const coll = db.jstests_find_and_modify_hint;

(function normalIndexTest() {
    // Hint using a key pattern.
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, x: 1, y: 1}));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: -1}));

    // Hint using index key pattern.
    let famUpdateCmd =
        {findAndModify: coll.getName(), query: {x: 1}, update: {$set: {y: 1}}, hint: {y: -1}};
    assertCommandUsesIndex(famUpdateCmd, {y: -1});

    // Hint using an index name.
    famUpdateCmd =
        {findAndModify: coll.getName(), query: {x: 1}, update: {$set: {y: 1}}, hint: 'y_-1'};
    assertCommandUsesIndex(famUpdateCmd, {y: -1});

    // Passing a hint should not use the idhack fast-path.
    famUpdateCmd =
        {findAndModify: coll.getName(), query: {_id: 1}, update: {$set: {y: 1}}, hint: 'y_-1'};
    assertCommandUsesIndex(famUpdateCmd, {y: -1});

    // Passing a hint with an empty 'update' object should work as expected.
    famUpdateCmd = {findAndModify: coll.getName(), query: {_id: 1}, update: {}, hint: 'y_-1'};
    assertCommandUsesIndex(famUpdateCmd, {y: -1});

    // Passing a hint on _id with an empty 'update' object with an IDHACK eligible query should
    // work as expected.
    famUpdateCmd = {findAndModify: coll.getName(), query: {_id: 1}, update: {}, hint: {_id: 1}};
    assertCommandUsesIndex(famUpdateCmd, {_id: 1});

    // Hint using an index name when removing documents.
    const famRemoveCmd =
        {findAndModify: coll.getName(), query: {_id: 1}, remove: true, hint: 'y_-1'};
    assertCommandUsesIndex(famRemoveCmd, {y: -1});
})();

(function sparseIndexTest() {
    // Create a sparse index which includes only 1 of the 3 documents in the collection.
    coll.drop();
    assert.commandWorked(coll.insert([{_id: 0, x: 1}, {_id: 1, x: 1}, {_id: 2, x: 1, s: 0}]));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({s: 1}, {sparse: true}));

    // Hint should be respected, even on sparse indexes.
    let famUpdateCmd =
        {findAndModify: coll.getName(), query: {_id: 1}, update: {$set: {y: 1}}, hint: {s: 1}};
    assertCommandUsesIndex(famUpdateCmd, {s: 1});

    // Update hinting a sparse index updates only the document in the sparse index.
    famUpdateCmd =
        {findAndModify: coll.getName(), query: {}, update: {$set: {y: 1}}, hint: {s: 1}, new: true};
    let res = assert.commandWorked(coll.runCommand(famUpdateCmd));
    assert.docEq(res.value, {_id: 2, x: 1, s: 0, y: 1});

    // Update hinting a sparse index with upsert option can result in an insert even if the
    // correct behaviour would be to update an existing document.
    assert.commandWorked(coll.insert({x: 2}));
    famUpdateCmd = {
        findAndModify: coll.getName(),
        query: {x: 2},
        update: {$set: {_id: 3, y: 1}},
        hint: {s: 1},
        upsert: true,
        new: true
    };
    res = assert.commandWorked(coll.runCommand(famUpdateCmd));
    assert.eq(res.lastErrorObject.upserted, 3);  // value of _id
    assert.docEq(res.value, {_id: 3, x: 2, y: 1});

    // Make sure an indexed document gets deleted when index hint is provided.
    assert.commandWorked(coll.insert({x: 1}));
    const famRemoveCmd = {findAndModify: coll.getName(), query: {x: 1}, remove: true, hint: {s: 1}};
    res = assert.commandWorked(coll.runCommand(famRemoveCmd));
    assert.docEq(res.value, {_id: 2, x: 1, s: 0, y: 1});
})();

(function shellHelpersTest() {
    coll.drop();
    assert.commandWorked(coll.insert([{_id: 0, x: 1}, {_id: 1, x: 1}, {_id: 2, x: 1, s: 0}]));
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({s: 1}, {sparse: true}));

    // Test shell helpers using a hinted sparse index should only update documents that exist in
    // the sparse index.
    let newDoc =
        coll.findAndModify({query: {x: 1}, update: {$set: {y: 2}}, hint: {s: 1}, new: true});
    assert.docEq(newDoc, {_id: 2, x: 1, s: 0, y: 2});

    // Insert document that will not be in the sparse index. Update hinting sparse index should
    // result in upsert.
    assert.commandWorked(coll.insert({_id: 3, x: 2}));
    newDoc = coll.findOneAndUpdate(
        {x: 2}, {$set: {_id: 4, y: 2}}, {hint: {s: 1}, upsert: true, returnNewDocument: true});
    assert.docEq(newDoc, {_id: 4, x: 2, y: 2});

    // Similarly, hinting the sparse index for a replacement should result in an upsert.
    assert.commandWorked(coll.insert({_id: 5, x: 3}));
    newDoc = coll.findOneAndReplace(
        {x: 3}, {_id: 6, y: 2}, {hint: {s: 1}, upsert: true, returnNewDocument: true});
    assert.docEq(newDoc, {_id: 6, y: 2});

    // Make sure an indexed document gets deleted when index hint is provided.
    newDoc = coll.findOneAndDelete({x: 2}, {hint: {s: 1}});
    assert.docEq(newDoc, {_id: 3, x: 2});
})();

(function failedHintTest() {
    coll.drop();
    assert.commandWorked(coll.insert({x: 1}));
    assert.commandWorked(coll.createIndex({x: 1}));

    // Command should fail with incorrectly formatted hints.
    let famUpdateCmd =
        {findAndModify: coll.getName(), query: {_id: 1}, update: {$set: {y: 1}}, hint: 1};
    assert.commandFailedWithCode(coll.runCommand(famUpdateCmd), ErrorCodes.FailedToParse);
    famUpdateCmd =
        {findAndModify: coll.getName(), query: {_id: 1}, update: {$set: {y: 1}}, hint: true};
    assert.commandFailedWithCode(coll.runCommand(famUpdateCmd), ErrorCodes.FailedToParse);

    // Command should fail with hints to non-existent indexes.
    famUpdateCmd = {
        findAndModify: coll.getName(),
        query: {_id: 1},
        update: {$set: {y: 1}},
        hint: {badHint: 1}
    };
    assert.commandFailedWithCode(coll.runCommand(famUpdateCmd), ErrorCodes.BadValue);
})();
})();
