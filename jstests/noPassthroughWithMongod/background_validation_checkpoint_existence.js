/**
 * Tests that validating a collection in the background that has not been checkpointed yet does no
 * validation. In addition, ensures that background validation skips indexes that are not yet
 * checkpointed.
 *
 * @tags: [requires_fsync, requires_wiredtiger, requires_persistence]
 */
(function() {
'use strict';

const collName = "background_validation_checkpoint_existence";

const forceCheckpoint = () => {
    assert.commandWorked(db.fsyncLock());
    assert.commandWorked(db.fsyncUnlock());
};

assert.commandWorked(db.createCollection(collName));
const coll = db.getCollection(collName);

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({x: i}));
}

// The collection has not been checkpointed yet, so there is nothing to validate.
let res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert.eq(true, res.valid);
assert.eq(false, res.hasOwnProperty("nrecords"));
assert.eq(false, res.hasOwnProperty("nIndexes"));

forceCheckpoint();

res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert.eq(true, res.valid);
assert.eq(true, res.hasOwnProperty("nrecords"));
assert.eq(true, res.hasOwnProperty("nIndexes"));

assert.commandWorked(coll.createIndex({x: 1}));

// Shouldn't validate the newly created index here as it wasn't checkpointed yet.
res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert.eq(true, res.valid);
assert.eq(1, res.nIndexes);

forceCheckpoint();

// Validating after the checkpoint should validate the newly created index.
res = assert.commandWorked(db.runCommand({validate: collName, background: true}));
assert.eq(true, res.valid);
assert.eq(2, res.nIndexes);
}());
