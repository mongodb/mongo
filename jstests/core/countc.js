// In fast count mode the Matcher is bypassed when matching can be performed by a BtreeCursor and
// its delegate FieldRangeVector or an IntervalBtreeCursor.  The tests below check that fast count
// mode is implemented appropriately in specific cases.
//
// SERVER-1752

t = db.jstests_countc;
t.drop();

// Match a subset of inserted values within a $in operator.
t.drop();
t.ensureIndex({a: 1});
// Save 'a' values 0, 0.5, 1.5, 2.5 ... 97.5, 98.5, 99.
t.save({a: 0});
t.save({a: 99});
for (i = 0; i < 99; ++i) {
    t.save({a: (i + 0.5)});
}
// Query 'a' values $in 0, 1, 2, ..., 99.
vals = [];
for (i = 0; i < 100; ++i) {
    vals.push(i);
}
// Only values 0 and 99 of the $in set are present in the collection, so the expected count is 2.
assert.eq(2, t.count({a: {$in: vals}}));

// Match 'a' values within upper and lower limits.
t.drop();
t.ensureIndex({a: 1});
t.save({a: [1, 2]});  // Will match because 'a' is in range.
t.save({a: 9});       // Will not match because 'a' is not in range.
// Only one document matches.
assert.eq(1, t.count({a: {$gt: 0, $lt: 5}}));

// Match two nested fields within an array.
t.drop();
t.ensureIndex({'a.b': 1, 'a.c': 1});
t.save({a: [{b: 2, c: 3}, {}]});
// The document does not match because its c value is 3.
assert.eq(0, t.count({'a.b': 2, 'a.c': 2}));

// $gt:string only matches strings.
t.drop();
t.ensureIndex({a: 1});
t.save({a: 'a'});  // Will match.
t.save({a: {}});   // Will not match because {} is not a string.
// Only one document matches.
assert.eq(1, t.count({a: {$gte: ''}}));

// $lte:date only matches dates.
t.drop();
t.ensureIndex({a: 1});
t.save({a: new Date(1)});  // Will match.
t.save({a: true});         // Will not match because 'true' is not a date.
// Only one document matches.
assert.eq(1, t.count({a: {$lte: new Date(1)}}));

// Querying for 'undefined' triggers an error.
t.drop();
t.ensureIndex({a: 1});
assert.throws(function() {
    t.count({a: undefined});
});

// Count using a descending order index.
t.drop();
t.ensureIndex({a: -1});
t.save({a: 1});
t.save({a: 2});
t.save({a: 3});
assert.eq(1, t.count({a: {$gt: 2}}));
assert.eq(1, t.count({a: {$lt: 2}}));
assert.eq(2, t.count({a: {$lte: 2}}));
assert.eq(2, t.count({a: {$lt: 3}}));

// Count using a compound index.
t.drop();
t.ensureIndex({a: 1, b: 1});
t.save({a: 1, b: 2});
t.save({a: 2, b: 1});
t.save({a: 2, b: 3});
t.save({a: 3, b: 4});
assert.eq(1, t.count({a: 1}));
assert.eq(2, t.count({a: 2}));
assert.eq(1, t.count({a: {$gt: 2}}));
assert.eq(1, t.count({a: {$lt: 2}}));
assert.eq(2, t.count({a: 2, b: {$gt: 0}}));
assert.eq(1, t.count({a: 2, b: {$lt: 3}}));
assert.eq(1, t.count({a: 1, b: {$lt: 3}}));

// Count using a compound descending order index.
t.drop();
t.ensureIndex({a: 1, b: -1});
t.save({a: 1, b: 2});
t.save({a: 2, b: 1});
t.save({a: 2, b: 3});
t.save({a: 3, b: 4});
assert.eq(1, t.count({a: {$gt: 2}}));
assert.eq(1, t.count({a: {$lt: 2}}));
assert.eq(2, t.count({a: 2, b: {$gt: 0}}));
assert.eq(1, t.count({a: 2, b: {$lt: 3}}));
assert.eq(1, t.count({a: 1, b: {$lt: 3}}));

// Count with a multikey value.
t.drop();
t.ensureIndex({a: 1});
t.save({a: [1, 2]});
assert.eq(1, t.count({a: {$gt: 0, $lte: 2}}));

// Count with a match constraint on an unindexed field.
t.drop();
t.ensureIndex({a: 1});
t.save({a: 1, b: 1});
t.save({a: 1, b: 2});
assert.eq(1, t.count({a: 1, $where: 'this.b == 1'}));
