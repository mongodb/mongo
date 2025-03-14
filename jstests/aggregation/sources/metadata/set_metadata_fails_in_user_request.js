// Tests that the $setMetadata stage is not allowed in user requests.
//
// @tags: [
//   requires_fcv_81,
// ]

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({a: 5, foo: "bar"}));

assert.throwsWithCode(() => coll.aggregate([{$setMetadata: {score: "$a"}}]), 5491300);
