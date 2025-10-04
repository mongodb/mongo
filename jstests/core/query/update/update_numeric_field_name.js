// Test that update operations correctly fail if they violate the "ambiguous field name in array"
// constraint for indexes. This is designed to reproduce SERVER-37058.
// @tags: [
//   uses_full_validation,
// ]
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({_id: 0, "a": [{}]}));
assert.commandWorked(coll.createIndex({"a.0.c": 1}));

// Attempt to insert a field name '0'. The first '0' refers to the first element of the array
// 'a'.
assert.commandFailedWithCode(coll.update({_id: 0}, {$set: {"a.0.0": 1}}), 16746);

// Verify that the indexes were not affected.
let res = assert.commandWorked(coll.validate({full: true}));
assert(res.valid, tojson(res));

assert.commandFailedWithCode(coll.update({_id: 0}, {$set: {"a.0.0.b": 1}}), 16746);
res = assert.commandWorked(coll.validate({full: true}));
assert(res.valid, tojson(res));

// An update which does not violate the ambiguous field name in array constraint should succeed.
assert.commandWorked(coll.update({_id: 0}, {$set: {"a.1.b.0.0": 1}}));

res = assert.commandWorked(coll.validate({full: true}));
assert(res.valid, tojson(res));
