// Test that write size estimation in mongos respects 'hint' field.
// @tags: [
//      requires_sharding,
//      # TODO (SPM-2077): remove "balancer off" tag once the project is over: migration can cause
//      # an index to be created before a dropIndex commits causing the metadata validation to fail.
//      assumes_balancer_off,
// ]
const coll = db.update_with_large_hint;
coll.drop();

const longHint = "x".repeat(1000);
assert.commandWorked(coll.createIndex({[longHint]: 1}));
assert.commandWorked(coll.insert({_id: 0}));

assert.commandWorked(coll.runCommand("update", {
    updates: [{q: {_id: 0}, u: {$set: {x: 1}}, hint: {[longHint]: 1}}],
}));

assert.commandWorked(coll.runCommand("delete", {
    deletes: [{q: {_id: 0}, limit: 1, hint: {[longHint]: 1}}],
}));

assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.insert({_id: 0}));

// Both commands should fail because hint does not correspond to the existing index.
assert.commandFailedWithCode(coll.runCommand("update", {
    updates: [{q: {_id: 0}, u: {$set: {x: 1}}, hint: {[longHint]: 1}}],
}),
                             ErrorCodes.BadValue);

assert.commandFailedWithCode(coll.runCommand("delete", {
    deletes: [{q: {_id: 0}, limit: 1, hint: {[longHint]: 1}}],
}),
                             ErrorCodes.BadValue);
