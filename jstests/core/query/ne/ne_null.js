// This is a test for the query correctness bug described in SERVER-36681. A {$ne: null} query
// should not return documents where the value doesn't exist
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({a: [1, {c: 1}]}));
assert.commandWorked(coll.insert({a: [1]}));
assert.commandWorked(coll.insert({a: []}));
assert.commandWorked(coll.insert({a: [[]]}));
assert.commandWorked(coll.insert({a: [{}]}));
assert.commandWorked(coll.insert({}));
assert.commandWorked(coll.insert({a: {}}));
assert.commandWorked(coll.insert({a: null}));

assert.eq(coll.count({b: {$ne: null}}), 0);
assert.eq(coll.count({"a.b": {$ne: null}}), 0);

assert.eq(coll.count({b: {$eq: null}}), 8);
assert.eq(coll.count({"a.b": {$eq: null}}), 8);
