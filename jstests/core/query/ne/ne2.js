// check that we don't scan $ne values
// @tags: [
//   assumes_read_concern_local,
// ]

let t = db.jstests_ne2;
t.drop();
t.createIndex({a: 1});

t.save({a: -0.5});
t.save({a: 0});
t.save({a: 0});
t.save({a: 0.5});

let e = t.find({a: {$ne: 0}}).explain(true);
assert.eq(2, e.executionStats.nReturned, "A");

e = t.find({a: {$gt: -1, $lt: 1, $ne: 0}}).explain(true);
assert.eq(2, e.executionStats.nReturned, "B");
