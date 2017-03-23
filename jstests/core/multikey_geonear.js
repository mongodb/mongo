// Test that we correct return results for compound 2d and 2dsphere indices in
// both the multikey and non-multikey cases.

var t = db.jstests_multikey_geonear;
t.drop();

// Check that the result set from the cursor is the document with _id: 2,
// followed by _id: 1, and _id: 0.
function checkResults(cursor) {
    for (var i = 2; i >= 0; i--) {
        assert.eq(i, cursor.next()["_id"]);
    }
    assert(!cursor.hasNext());
}

t.ensureIndex({a: 1, b: "2dsphere"});

t.insert({_id: 0, a: 0, b: {type: "Point", coordinates: [0, 0]}});
t.insert({_id: 1, a: 0, b: {type: "Point", coordinates: [1, 1]}});
t.insert({_id: 2, a: 0, b: {type: "Point", coordinates: [2, 2]}});

// Ensure that the results are returned sorted by increasing distance.
var cursor = t.find({a: {$gte: 0}, b: {$near: {$geometry: {type: "Point", coordinates: [2, 2]}}}});
checkResults(cursor);

// The results should be the same if we make the "a" field multikey.
t.insert({_id: 3, a: ["multi", "key"], b: {type: "Point", coordinates: [-1, -1]}});
cursor = t.find({a: {$gte: 0}, b: {$near: {$geometry: {type: "Point", coordinates: [2, 2]}}}});
checkResults(cursor);

// Repeat these tests for a 2d index.
t.drop();
t.ensureIndex({a: "2d", b: 1});
t.insert({_id: 0, a: [0, 0], b: 0});
t.insert({_id: 1, a: [1, 1], b: 1});
t.insert({_id: 2, a: [2, 2], b: 2});

cursor = t.find({a: {$near: [2, 2]}, b: {$gte: 0}});
checkResults(cursor);

t.insert({_id: 3, a: [-1, -1], b: ["multi", "key"]});
cursor = t.find({a: {$near: [2, 2]}, b: {$gte: 0}});
checkResults(cursor);

// The fields in the compound 2dsphere index share a prefix.
t.drop();
t.ensureIndex({"a.b": 1, "a.c": "2dsphere"});
t.insert({_id: 0, a: [{b: 0}, {c: {type: "Point", coordinates: [0, 0]}}]});
t.insert({_id: 1, a: [{b: 1}, {c: {type: "Point", coordinates: [1, 1]}}]});
t.insert({_id: 2, a: [{b: 2}, {c: {type: "Point", coordinates: [2, 2]}}]});

cursor =
    t.find({"a.b": {$gte: 0}, "a.c": {$near: {$geometry: {type: "Point", coordinates: [2, 2]}}}});
checkResults(cursor);

// Double check that we're not intersecting bounds. Doing so should cause us to
// miss the result here.
t.insert({_id: 3, a: [{b: 10}, {b: -1}, {c: {type: "Point", coordinates: [0, 0]}}]});
cursor = t.find(
    {"a.b": {$lt: 0, $gt: 9}, "a.c": {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}}});
assert.eq(3, cursor.next()["_id"]);
assert(!cursor.hasNext());

// The fields in the compound 2d index share a prefix.
t.drop();
t.ensureIndex({"a.b": "2d", "a.c": 1});
t.insert({_id: 0, a: [{b: [0, 0]}, {c: 0}]});
t.insert({_id: 1, a: [{b: [1, 1]}, {c: 1}]});
t.insert({_id: 2, a: [{b: [2, 2]}, {c: 2}]});

cursor = t.find({"a.b": {$near: [2, 2]}, "a.c": {$gte: 0}});
checkResults(cursor);
