// Test basic NaN handling.

var t = db.jstests_nan;
t.drop();

var cursor;

t.insert({_id: 0, a: -Infinity});
t.insert({_id: 1, a: -3});
t.insert({_id: 2, a: 0});
t.insert({_id: 3, a: 3});
t.insert({_id: 4, a: Infinity});
t.insert({_id: 5, a: NaN});
t.insert({_id: 6, a: undefined});
t.insert({_id: 7, a: null});
t.insert({_id: 8, a: []});
t.insert({_id: 9, a: {b: 1}});
t.insert({_id: 10, a: {b: 1}});

/**
 * Ensures correct results for EQ, LT, LTE, GT, and GTE cases.
 */
var testNaNComparisons = function() {
    // EQ
    cursor = t.find({a: NaN});
    assert.eq(5, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    // LT
    cursor = t.find({a: {$lt: NaN}});
    assert(!cursor.hasNext());

    // LTE
    cursor = t.find({a: {$lte: NaN}});
    assert.eq(5, cursor.next()["_id"]);
    assert(!cursor.hasNext());

    // GT
    cursor = t.find({a: {$gt: NaN}});
    assert(!cursor.hasNext());

    // GTE
    cursor = t.find({a: {$gte: NaN}});
    assert.eq(5, cursor.next()["_id"]);
    assert(!cursor.hasNext());
};

// Unindexed
testNaNComparisons();

// Indexed
t.ensureIndex({a: 1});
testNaNComparisons();
