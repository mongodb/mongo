// Test that an aggregate command where the "pipeline" field has the wrong type fails with a
// TypeMismatch error.
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({}));

assert.commandFailedWithCode(db.runCommand({aggregate: coll.getName(), pipeline: 1}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(db.runCommand({aggregate: coll.getName(), pipeline: {}}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(db.runCommand({aggregate: coll.getName(), pipeline: [1, 2]}),
                             ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(db.runCommand({aggregate: coll.getName(), pipeline: [1, null]}),
                             ErrorCodes.TypeMismatch);