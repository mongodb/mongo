// Tests that the $setMetadata stage is not allowed in user requests.

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({a: 5, foo: "bar"}));

assert.throwsWithCode(() => coll.aggregate([{$setMetadata: {score: "$a"}}]), 5491300);
