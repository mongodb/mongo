/**
 * Regression test for a crash (invariant failure) when using $_internalIndexKey with compound
 * wildcard indexes.
 */
const conn = MongoRunner.runMongod();
const coll = conn.getDB("test").compound_wildcard_index_key;

function getKeys(spec) {
    return coll.aggregate([{$replaceRoot: {newRoot: {_id: {$_internalIndexKey: {doc: "$$ROOT", spec}}}}}]).toArray()[0]
        ._id;
}

coll.drop();
coll.insertOne({a: 1, b: {x: 2, y: 3}, other: 9});

assert.sameMembers(getKeys({key: {"$**": 1, a: 1}, name: "t1", wildcardProjection: {a: 0}}), [
    {"b.x": 2, a: 1},
    {"b.y": 3, a: 1},
    {other: 9, a: 1},
]);

assert.sameMembers(getKeys({key: {a: 1, "$**": 1}, name: "t2", wildcardProjection: {a: 0}}), [
    {a: 1, "b.x": 2},
    {a: 1, "b.y": 3},
    {a: 1, other: 9},
]);

assert.eq(getKeys({key: {"$**": 1}, name: "t3"}).length, 4);

assert.sameMembers(getKeys({key: {"b.$**": 1}, name: "t3b"}), [{"b.x": 2}, {"b.y": 3}]);

assert.sameMembers(getKeys({key: {a: 1, "b.$**": 1}, name: "t3c"}), [
    {a: 1, "b.x": 2},
    {a: 1, "b.y": 3},
]);

assert.sameMembers(getKeys({key: {a: 1, other: 1, "$**": 1}, name: "t3d", wildcardProjection: {a: 0, other: 0}}), [
    {a: 1, other: 9, "b.x": 2},
    {a: 1, other: 9, "b.y": 3},
]);

assert.sameMembers(getKeys({key: {"b.x": 1, "$**": 1, "b.y": 1}, name: "t3d", wildcardProjection: {b: 0}}), [
    {"b.x": 2, "a": 1, "b.y": 3},
    {"b.x": 2, "other": 9, "b.y": 3},
]);

assert.sameMembers(getKeys({key: {"$**": 1}, name: "t3d", wildcardProjection: {a: 1, other: 1}}), [{a: 1}, {other: 9}]);

assert.sameMembers(getKeys({key: {"$**": 1}, name: "t3d", wildcardProjection: {b: 1}}), [{"b.x": 2}, {"b.y": 3}]);

assert.sameMembers(getKeys({key: {"$**": 1}, name: "t3d", wildcardProjection: {a: 0, other: 0}}), [
    {"b.x": 2},
    {"b.y": 3},
]);

coll.drop();
coll.insertOne({a: 5, b: [{x: 2}, {x: 3}, {y: 4}]});
assert.sameMembers(getKeys({key: {a: 1, "$**": 1}, name: "t4", wildcardProjection: {a: 0}}), [
    {a: 5, "b.x": 2},
    {a: 5, "b.x": 3},
    {a: 5, "b.y": 4},
]);

MongoRunner.stopMongod(conn);
