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

    const coll = db.jstests_update_hint;
    coll.drop();

    assert.commandWorked(coll.insert({x: 1, y: 1}));
    assert.commandWorked(coll.insert({x: 1, y: 1}));

    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: -1}));

    function assertCommandUsesIndex(command, expectedHintKeyPattern) {
        const out = assert.commandWorked(coll.runCommand({explain: command}));
        const planStage = getPlanStage(out, "IXSCAN");
        assert.neq(null, planStage);
        assert.eq(planStage.keyPattern, expectedHintKeyPattern, tojson(planStage));
    }

    const updateCmd = {
        update: 'jstests_update_hint',
    };

    const updates = [{q: {x: 1}, u: {$set: {y: 1}}, hint: {x: 1}}];

    updateCmd.updates = updates;
    // Hint using a key pattern.
    assertCommandUsesIndex(updateCmd, {x: 1});

    // Hint using an index name.
    updates[0].hint = 'y_-1';
    assertCommandUsesIndex(updateCmd, {y: -1});

    // Passing a hint should not use the idhack fast-path.
    updates[0].q = {_id: 1};
    assertCommandUsesIndex(updateCmd, {y: -1});

    // Create a sparse index.
    assert.commandWorked(coll.createIndex({s: 1}, {sparse: true}));

    // Hint should be respected, even on incomplete indexes.
    updates[0].hint = {s: 1};
    assertCommandUsesIndex(updateCmd, {s: 1});

    // Command should fail with incorrectly formatted hints.
    updates[0].hint = 1;
    assert.commandFailedWithCode(coll.runCommand(updateCmd), ErrorCodes.FailedToParse);
    updates[0].hint = true;
    assert.commandFailedWithCode(coll.runCommand(updateCmd), ErrorCodes.FailedToParse);

    // Command should fail with hints to non-existent indexes.
    updates[0].hint = {badHint: 1};
    assert.commandFailedWithCode(coll.runCommand(updateCmd), ErrorCodes.BadValue);

    // Insert document that will be in the sparse index.
    assert.commandWorked(coll.insert({x: 1, s: 0}));

    // Update hinting a sparse index updates only the document in the sparse index.
    updates[0] = {q: {}, u: {$set: {s: 1}}, hint: {s: 1}};
    assert.commandWorked(coll.runCommand(updateCmd));
    assert.eq(1, coll.count({s: 1}));

    // Update hinting a sparse index with upsert option can result in an insert even if the correct
    // behaviour would be to update an existing document.
    assert.commandWorked(coll.insert({x: 2}));
    updates[0] = {q: {x: 2}, u: {$set: {s: 1}}, hint: {s: 1}, upsert: true};
    assert.commandWorked(coll.runCommand(updateCmd));
    assert.eq(2, coll.count({x: 2}));

})();
