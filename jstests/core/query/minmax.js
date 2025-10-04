// Test min / max query parameters.
// @tags: [
//  assumes_balancer_off,
//  requires_getmore,
// ]
import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.jstests_minmax;
coll.drop();

function addData() {
    assert.commandWorked(coll.save({a: 1, b: 1}));
    assert.commandWorked(coll.save({a: 1, b: 2}));
    assert.commandWorked(coll.save({a: 2, b: 1}));
    assert.commandWorked(coll.save({a: 2, b: 2}));
}

assert.commandWorked(coll.createIndex({a: 1, b: 1}));
addData();

assert.eq(1, coll.find().hint({a: 1, b: 1}).min({a: 1, b: 2}).max({a: 2, b: 1}).toArray().length);
assert.eq(2, coll.find().hint({a: 1, b: 1}).min({a: 1, b: 2}).max({a: 2, b: 1.5}).toArray().length);
assert.eq(2, coll.find().hint({a: 1, b: 1}).min({a: 1, b: 2}).max({a: 2, b: 2}).toArray().length);

// Single bound.
assert.eq(3, coll.find().hint({a: 1, b: 1}).min({a: 1, b: 2}).toArray().length);
assert.eq(3, coll.find().hint({a: 1, b: 1}).max({a: 2, b: 1.5}).toArray().length);
assert.eq(3, coll.find().hint({a: 1, b: 1}).min({a: 1, b: 2}).hint({a: 1, b: 1}).toArray().length);
assert.eq(3, coll.find().hint({a: 1, b: 1}).max({a: 2, b: 1.5}).hint({a: 1, b: 1}).toArray().length);

coll.drop();
assert.commandWorked(coll.createIndex({a: 1, b: -1}));
addData();
assert.eq(4, coll.find().hint({a: 1, b: -1}).min({a: 1, b: 2}).toArray().length);
assert.eq(4, coll.find().hint({a: 1, b: -1}).max({a: 2, b: 0.5}).toArray().length);
assert.eq(1, coll.find().hint({a: 1, b: -1}).min({a: 2, b: 1}).toArray().length);
assert.eq(1, coll.find().hint({a: 1, b: -1}).max({a: 1, b: 1.5}).toArray().length);
assert.eq(4, coll.find().hint({a: 1, b: -1}).min({a: 1, b: 2}).hint({a: 1, b: -1}).toArray().length);
assert.eq(4, coll.find().hint({a: 1, b: -1}).max({a: 2, b: 0.5}).hint({a: 1, b: -1}).toArray().length);
assert.eq(1, coll.find().hint({a: 1, b: -1}).min({a: 2, b: 1}).hint({a: 1, b: -1}).toArray().length);
assert.eq(1, coll.find().hint({a: 1, b: -1}).max({a: 1, b: 1.5}).hint({a: 1, b: -1}).toArray().length);

// Check that min/max requires a hint.
assert.throwsWithCode(
    () => coll.find().min({a: 1, b: 2}).max({a: 2, b: 1}).toArray(),
    [ErrorCodes.NoQueryExecutionPlans, 51173],
);

// Hint doesn't match.
let error = assert.throws(function () {
    coll.find().min({a: 1}).hint({a: 1, b: -1}).toArray();
});
assert.eq(error.code, 51174, error);

error = assert.throws(function () {
    coll.find().min({a: 1, b: 1}).max({a: 1}).hint({a: 1, b: -1}).toArray();
});
assert.eq(error.code, 51176, error);

error = assert.throws(function () {
    coll.find().min({b: 1}).max({a: 1, b: 2}).hint({a: 1, b: -1}).toArray();
});
assert.eq(error.code, 51176, error);

// No query solutions.
error = assert.throws(function () {
    coll.find().min({a: 1}).hint({$natural: 1}).toArray();
});
assert.eq(error.code, ErrorCodes.NoQueryExecutionPlans, error);

assert.throwsWithCode(function () {
    coll.find().max({a: 1}).hint({$natural: 1}).toArray();
}, ErrorCodes.NoQueryExecutionPlans);

coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.save({_id: i, a: i}));
}

// Reverse direction scan of the a:1 index between a:6 (inclusive) and a:3 (exclusive) is
// expected to fail, as max must be > min.
assert.throwsWithCode(function () {
    coll.find().hint({a: 1}).min({a: 6}).max({a: 3}).sort({a: -1}).toArray();
}, 51175);

// A find with identical min and max values is expected to fail, as max is exclusive.
assert.throwsWithCode(function () {
    coll.find().hint({a: 1}).min({a: 2}).max({a: 2}).toArray();
}, 51175);

assert.throwsWithCode(function () {
    coll.find().hint({a: 1}).min({a: 2}).max({a: 2}).sort({a: -1}).toArray();
}, 51175);

coll.drop();
addData();
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

assert.throwsWithCode(function () {
    coll.find().min({a: 1, b: 2}).max({a: 1, b: 2}).hint({a: 1, b: 1}).toArray();
}, 51175);

// Test ascending index.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({a: 3}));
assert.commandWorked(coll.insert({a: 4}));
assert.commandWorked(coll.insert({a: 5}));

let cursor = coll.find().hint({a: 1}).min({a: 4});
if (FixtureHelpers.numberOfShardsForCollection(coll) === 1) {
    assert.eq(4, cursor.next().a);
    assert.eq(5, cursor.next().a);
} else {
    // With more than one shard, we cannot assume the results will come back in order, since we
    // did not request a sort.
    assert(resultsEq([cursor.next().a, cursor.next().a], [4, 5]));
}
assert(!cursor.hasNext());

cursor = coll.find().hint({a: 1}).max({a: 4});
assert.eq(3, cursor.next()["a"]);
assert(!cursor.hasNext());

// Test descending index.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: -1}));

cursor = coll.find().hint({a: -1}).min({a: 4});
if (FixtureHelpers.numberOfShardsForCollection(coll) === 1) {
    assert.eq(4, cursor.next().a);
    assert.eq(3, cursor.next().a);
} else {
    // With more than one shard, we cannot assume the results will come back in order, since we
    // did not request a sort.
    assert(resultsEq([cursor.next().a, cursor.next().a], [4, 3]));
}
assert(!cursor.hasNext());

cursor = coll.find().hint({a: -1}).max({a: 4});
assert.eq(5, cursor.next()["a"]);
assert(!cursor.hasNext());
