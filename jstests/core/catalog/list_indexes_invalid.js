// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]

// Test for invalid values of "cursor" and "cursor.batchSize".

let coll = db.list_indexes_invalid;
coll.drop();

assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));

let getListIndexesCursor = function (coll, options, subsequentBatchSize) {
    return new DBCommandCursor(coll.getDB(), coll.runCommand("listIndexes", options), subsequentBatchSize);
};

assert.throws(function () {
    getListIndexesCursor(coll, {cursor: 0});
});
assert.throws(function () {
    getListIndexesCursor(coll, {cursor: "x"});
});
assert.throws(function () {
    getListIndexesCursor(coll, {cursor: []});
});
assert.throws(function () {
    getListIndexesCursor(coll, {cursor: {foo: 1}});
});
assert.throws(function () {
    getListIndexesCursor(coll, {cursor: {batchSize: -1}});
});
assert.throws(function () {
    getListIndexesCursor(coll, {cursor: {batchSize: "x"}});
});
assert.throws(function () {
    getListIndexesCursor(coll, {cursor: {batchSize: {}}});
});
assert.throws(function () {
    getListIndexesCursor(coll, {cursor: {batchSize: 2, foo: 1}});
});
