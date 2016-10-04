// Test for invalid values of "cursor" and "cursor.batchSize".

var coll = db.list_indexes_invalid;
coll.drop();

assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.commandWorked(coll.ensureIndex({a: 1}, {unique: true}));

assert.throws(function() {
    getListIndexesCursor(coll, {cursor: 0});
});
assert.throws(function() {
    getListIndexesCursor(coll, {cursor: 'x'});
});
assert.throws(function() {
    getListIndexesCursor(coll, {cursor: []});
});
assert.throws(function() {
    getListIndexesCursor(coll, {cursor: {foo: 1}});
});
assert.throws(function() {
    getListIndexesCursor(coll, {cursor: {batchSize: -1}});
});
assert.throws(function() {
    getListIndexesCursor(coll, {cursor: {batchSize: 'x'}});
});
assert.throws(function() {
    getListIndexesCursor(coll, {cursor: {batchSize: {}}});
});
assert.throws(function() {
    getListIndexesCursor(coll, {cursor: {batchSize: 2, foo: 1}});
});
