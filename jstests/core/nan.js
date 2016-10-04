// Test basic NaN handling.

var t = db.jstests_nan;
t.drop();

var cursor;

t.insert({_id: 0, a: -Infinity});
t.insert({_id: 1, a: -3});
t.insert({_id: 2, a: 0});
t.insert({_id: 3, a: 3});
t.insert({_id: 4, a: Infinity});
// WiredTiger indexes handle -NaN and NaN differently.
t.insert({_id: 5, a: NaN});
t.insert({_id: 6, a: -NaN});
t.insert({_id: 7, a: undefined});
t.insert({_id: 8, a: null});
t.insert({_id: 9, a: []});
t.insert({_id: 10, a: {b: 1}});
t.insert({_id: 11, a: {b: 1}});

/**
 * Ensures correct results for EQ, LT, LTE, GT, and GTE cases.
 */
var testNaNComparisons = function() {
    // EQ
    cursor = t.find({a: NaN});
    assert.eq(5, cursor.next()["_id"]);
    assert.eq(6, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    // LT
    cursor = t.find({a: {$lt: NaN}});
    assert(!cursor.hasNext());

    // LTE
    cursor = t.find({a: {$lte: NaN}});
    var id1 = cursor.next()["_id"];
    var id2 = cursor.next()["_id"];
    // Exactly one of the two conditions must be true.
    var cond1 = id1 === 5 && id2 === 6;
    var cond2 = id1 === 6 && id2 === 5;
    assert(cond1 ^ cond2);
    assert(!cursor.hasNext());

    // GT
    cursor = t.find({a: {$gt: NaN}});
    assert(!cursor.hasNext());

    // GTE
    cursor = t.find({a: {$gte: NaN}});
    var id1 = cursor.next()["_id"];
    var id2 = cursor.next()["_id"];
    // Exactly one of the two conditions must be true.
    var cond1 = id1 === 5 && id2 === 6;
    var cond2 = id1 === 6 && id2 === 5;
    assert(cond1 ^ cond2);
    assert(!cursor.hasNext());
};

// Unindexed
testNaNComparisons();

// Indexed
t.ensureIndex({a: 1});
testNaNComparisons();
