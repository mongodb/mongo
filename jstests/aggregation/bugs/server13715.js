// Make sure that aggregation and query agree on how NO_BLOCKING_SORT should fail
// if only blocking sort solutions are available.

var t = db.jstests_server13715;
t.drop();

t.save({_id: 0, name: "red", value: 2});
t.save({_id: 1, name: "blue", value: 1});

var cursor = t.aggregate([{$match: {$or: [{name: "red"}, {name: "blue"}]}}, {$sort: {value: 1}}]);
assert.eq(1, cursor.next()["_id"]);
assert.eq(0, cursor.next()["_id"]);

// Repeat the test with an index.
t.ensureIndex({name: 1});

cursor = t.aggregate([{$match: {$or: [{name: "red"}, {name: "blue"}]}}, {$sort: {value: 1}}]);
assert.eq(1, cursor.next()["_id"]);
assert.eq(0, cursor.next()["_id"]);
