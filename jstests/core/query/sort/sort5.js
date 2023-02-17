(function() {
// test compound sorting
const t = db.sort5;
t.drop();

assert.commandWorked(t.insert({_id: 5, x: 1, y: {a: 5, b: 4}}));
assert.commandWorked(t.insert({_id: 7, x: 2, y: {a: 7, b: 3}}));
assert.commandWorked(t.insert({_id: 2, x: 3, y: {a: 2, b: 3}}));
assert.commandWorked(t.insert({_id: 9, x: 4, y: {a: 9, b: 3}}));

function testSortAndSortWithLimit(expected, sortPattern, description) {
    // Test sort and sort with limit.
    assert.eq(expected,
              t.find().sort(sortPattern).map(function(z) {
                  return z.x;
              }),
              description);

    assert.eq(expected,
              t.find().sort(sortPattern).limit(500).map(function(z) {
                  return z.x;
              }),
              description);
}

testSortAndSortWithLimit([4, 2, 3, 1], {"y.b": 1, "y.a": -1}, "A no index");
t.createIndex({"y.b": 1, "y.a": -1});
testSortAndSortWithLimit([4, 2, 3, 1], {"y.b": 1, "y.a": -1}, "A index");
assert(t.validate().valid, "A valid");

// test sorting on compound key involving _id

testSortAndSortWithLimit([4, 2, 3, 1], {"y.b": 1, _id: -1}, "B no index");
t.createIndex({"y.b": 1, "_id": -1});
testSortAndSortWithLimit([4, 2, 3, 1], {"y.b": 1, _id: -1}, "B index");
assert(t.validate().valid, "B valid");

function testWithProj(proj, sort, expected) {
    assert.eq(t.find({}, proj).sort(sort).map((doc) => doc._id), expected);
    assert.eq(t.find({}, proj).sort(sort).limit(500).map((doc) => doc._id), expected);
}
testWithProj({"y.a": 1, "y.b": 1, _id: 1}, {"y.a": 1, "y.b": 1, _id: 1}, [2, 5, 7, 9]);
})();
