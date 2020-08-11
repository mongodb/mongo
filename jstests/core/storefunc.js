// @tags: [
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   requires_fastcount,
// ]

// Use a private sister database to avoid conflicts with other tests that use system.js
var testdb = db.getSiblingDB("storefunc");
var res;

s = testdb.system.js;
s.remove({});
assert.eq(0, s.count(), "setup - A");

res = s.save({_id: "x", value: "3"});
assert(!res.hasWriteError(), "setup - B");
assert.eq(1, s.count(), "setup - C");

s.remove({_id: "x"});
assert.eq(0, s.count(), "setup - D");
s.save({_id: "x", value: "4"});
assert.eq(1, s.count(), "setup - E");

assert.eq(4, s.findOne({_id: "x"}).value, "E2 ");

assert.eq(4, s.findOne().value, "setup - F");
s.update({_id: "x"}, {$set: {value: 5}});
assert.eq(1, s.count(), "setup - G");
assert.eq(5, s.findOne().value, "setup - H");

s.update({_id: "x"}, {$set: {value: 6}});
assert.eq(1, s.count(), "setup2 - A");
assert.eq(6, s.findOne().value, "setup - B");

assert(s.getIndexKeys().length > 0, "no indexes");
assert(s.getIndexKeys()[0]._id, "no _id index");
