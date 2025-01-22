// Make sure that aggregation and query agree on how NO_BLOCKING_SORT should fail
// if only blocking sort solutions are available.

const t = db[jsTestName()];
t.drop();

assert.commandWorked(
    t.insertMany([{_id: 0, name: "red", value: 2}, {_id: 1, name: "blue", value: 1}]));

let cursor = t.aggregate([{$match: {$or: [{name: "red"}, {name: "blue"}]}}, {$sort: {value: 1}}]);
assert.eq(1, cursor.next()["_id"]);
assert.eq(0, cursor.next()["_id"]);

// Repeat the test with an index.
assert.commandWorked(t.createIndex({name: 1}));

cursor = t.aggregate([{$match: {$or: [{name: "red"}, {name: "blue"}]}}, {$sort: {value: 1}}]);
assert.eq(1, cursor.next()["_id"]);
assert.eq(0, cursor.next()["_id"]);
