// Tests that $_internalHybridSearch is not allowed in user requests.
//
// @tags: [
//   requires_fcv_90,
// ]

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({a: 1}));

assert.throwsWithCode(() => coll.aggregate([{$_internalHybridSearch: {}}]), 5491300);
