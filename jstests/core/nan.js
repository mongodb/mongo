/**
 * Tests basic NaN handling. Note that WiredTiger indexes handle -NaN and NaN differently.
 */
(function() {
    "use strict";

    const coll = db.jstests_nan;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, a: -Infinity}));
    assert.writeOK(coll.insert({_id: 1, a: -3}));
    assert.writeOK(coll.insert({_id: 2, a: 0}));
    assert.writeOK(coll.insert({_id: 3, a: 3}));
    assert.writeOK(coll.insert({_id: 4, a: Infinity}));
    assert.writeOK(coll.insert({_id: 5, a: NaN}));
    assert.writeOK(coll.insert({_id: 6, a: -NaN}));
    assert.writeOK(coll.insert({_id: 7, a: undefined}));
    assert.writeOK(coll.insert({_id: 8, a: null}));
    assert.writeOK(coll.insert({_id: 9, a: []}));
    assert.writeOK(coll.insert({_id: 10, a: {b: 1}}));
    assert.writeOK(coll.insert({_id: 11, a: {b: 1}}));

    /**
     * Ensures correct results for EQ, LT, LTE, GT, and GTE cases.
     */
    var testNaNComparisons = function() {
        // EQ
        let cursor = coll.find({a: NaN}).sort({_id: 1});
        assert.eq(5, cursor.next()["_id"]);
        assert.eq(6, cursor.next()["_id"]);
        assert(!cursor.hasNext());

        // LT
        cursor = coll.find({a: {$lt: NaN}});
        assert(!cursor.hasNext());

        // LTE
        cursor = coll.find({a: {$lte: NaN}}).sort({_id: 1});
        assert.eq(5, cursor.next()["_id"]);
        assert.eq(6, cursor.next()["_id"]);
        assert(!cursor.hasNext());

        // GT
        cursor = coll.find({a: {$gt: NaN}});
        assert(!cursor.hasNext());

        // GTE
        cursor = coll.find({a: {$gte: NaN}}).sort({_id: 1});
        assert.eq(5, cursor.next()["_id"]);
        assert.eq(6, cursor.next()["_id"]);
        assert(!cursor.hasNext());
    };

    // Unindexed.
    testNaNComparisons();

    // Indexed.
    assert.commandWorked(coll.createIndex({a: 1}));
    testNaNComparisons();
}());
