/*
 * Test that .min() and .max() queries properly handle the edge cases with NaN and Infinity.
 * Other edge cases are covered by C++ unit tests.
 */

var t = db.minmax_edge;

/*
 * Function to verify that the results of a query match the expected results.
 * Results is the cursor toArray, expectedIds is a list of _ids
 */
function verifyResultIds(results, expectedIds) {
    // check they are the same length
    assert.eq(results.length, expectedIds.length);

    function compare(a, b) {
        if (a._id < b._id)
            return -1;
        if (a._id > b._id)
            return 1;
        return 0;
    }

    results.sort(compare);
    expectedIds.sort();

    for (var i = 0; i < results.length; i++) {
        assert.eq(results._id, expectedIds._ids);
    }
}

/*
 * Shortcut to drop the collection and insert these 3 test docs. Used to change the indices
 * regardless of any previous indices.
 */
function reset(t) {
    t.drop();
    assert.writeOK(t.insert({_id: 0, a: 1, b: 1}));
    assert.writeOK(t.insert({_id: 1, a: 1, b: 2}));
    assert.writeOK(t.insert({_id: 2, a: 1, b: 3}));

    assert.writeOK(t.insert({_id: 3, a: 2, b: 1}));
    assert.writeOK(t.insert({_id: 4, a: 2, b: 2}));
    assert.writeOK(t.insert({_id: 5, a: 2, b: 3}));

    assert.writeOK(t.insert({_id: 6, a: 3, b: 1}));
    assert.writeOK(t.insert({_id: 7, a: 3, b: 2}));
    assert.writeOK(t.insert({_id: 8, a: 3, b: 3}));
}

// Two helpers to save typing
function verifyMin(minDoc, expectedIds) {
    verifyResultIds(t.find().min(minDoc).toArray(), expectedIds);
}

function verifyMax(minDoc, expectedIds) {
    verifyResultIds(t.find().max(minDoc).toArray(), expectedIds);
}

// Basic ascending index.
reset(t);
assert.commandWorked(t.ensureIndex({a: 1}));

verifyMin({a: Infinity}, []);
verifyMax({a: Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: -Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: -Infinity}, []);

// NaN < all ints.
verifyMin({a: NaN}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: NaN}, []);

// {a: 1} > all ints.
verifyMin({a: {a: 1}}, []);
verifyMax({a: {a: 1}}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

// 'a' > all ints.
verifyMin({a: 'a'}, []);
verifyMax({a: 'a'}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyResultIds(t.find().min({a: 4}).max({a: 4}).toArray(), []);

// Now with a compound index.
reset(t);
assert.commandWorked(t.ensureIndex({a: 1, b: -1}));

// Same as single-key index assertions, with b field present.
verifyMin({a: NaN, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: NaN, b: 1}, []);

verifyMin({a: Infinity, b: 1}, []);
verifyMax({a: Infinity, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: -Infinity, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: -Infinity, b: 1}, []);

verifyMin({a: {a: 1}, b: 1}, []);
verifyMax({a: {a: 1}, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: 'a', b: 1}, []);
verifyMax({a: 'a', b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyResultIds(t.find().min({a: 4, b: 1}).max({a: 4, b: 1}).toArray(), []);

// Edge cases on b values
verifyMin({a: 1, b: Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMin({a: 2, b: Infinity}, [3, 4, 5, 6, 7, 8]);
verifyMin({a: 3, b: Infinity}, [6, 7, 8]);
verifyMax({a: 1, b: Infinity}, []);
verifyMax({a: 2, b: Infinity}, [0, 1, 2]);
verifyMax({a: 3, b: Infinity}, [0, 1, 2, 3, 4, 5]);

verifyMin({a: 1, b: -Infinity}, [3, 4, 5, 6, 7, 8]);
verifyMin({a: 2, b: -Infinity}, [6, 7, 8]);
verifyMin({a: 3, b: -Infinity}, []);
verifyMax({a: 1, b: -Infinity}, [0, 1, 2]);
verifyMax({a: 2, b: -Infinity}, [0, 1, 2, 3, 4, 5]);
verifyMax({a: 3, b: -Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: 2, b: NaN}, [6, 7, 8]);
verifyMax({a: 2, b: NaN}, [0, 1, 2, 3, 4, 5]);

verifyMin({a: 2, b: {b: 1}}, [3, 4, 5, 6, 7, 8]);
verifyMax({a: 2, b: {b: 1}}, [0, 1, 2]);

verifyMin({a: 2, b: 'b'}, [3, 4, 5, 6, 7, 8]);
verifyMax({a: 2, b: 'b'}, [0, 1, 2]);

// Test descending index.
reset(t);
t.ensureIndex({a: -1});

verifyMin({a: NaN}, []);
verifyMax({a: NaN}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: Infinity}, []);

verifyMin({a: -Infinity}, []);
verifyMax({a: -Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: {a: 1}}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: {a: 1}}, []);

verifyMin({a: 'a'}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: 'a'}, []);

verifyResultIds(t.find().min({a: 4}).max({a: 4}).toArray(), []);

// Now with a compound index.
reset(t);
t.ensureIndex({a: -1, b: -1});

// Same as single-key index assertions, with b field present.
verifyMin({a: NaN, b: 1}, []);
verifyMax({a: NaN, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: Infinity, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: Infinity, b: 1}, []);

verifyMin({a: -Infinity, b: 1}, []);
verifyMax({a: -Infinity, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

verifyMin({a: {a: 1}, b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: {a: 1}, b: 1}, []);

verifyMin({a: 'a', b: 1}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: 'a', b: 1}, []);

// Edge cases on b values.
verifyMin({a: 1, b: Infinity}, [0, 1, 2]);
verifyMin({a: 2, b: Infinity}, [0, 1, 2, 3, 4, 5]);
verifyMin({a: 3, b: Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: 1, b: Infinity}, [3, 4, 5, 6, 7, 8]);
verifyMax({a: 2, b: Infinity}, [6, 7, 8]);
verifyMax({a: 3, b: Infinity}, []);

verifyMin({a: 1, b: -Infinity}, []);
verifyMin({a: 2, b: -Infinity}, [0, 1, 2]);
verifyMin({a: 3, b: -Infinity}, [0, 1, 2, 3, 4, 5]);
verifyMax({a: 1, b: -Infinity}, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
verifyMax({a: 2, b: -Infinity}, [3, 4, 5, 6, 7, 8]);
verifyMax({a: 3, b: -Infinity}, [6, 7, 8]);

verifyMin({a: 2, b: NaN}, [0, 1, 2]);
verifyMax({a: 2, b: NaN}, [3, 4, 5, 6, 7, 8]);

verifyMin({a: 2, b: {b: 1}}, [3, 4, 5, 6, 7, 8]);
verifyMax({a: 2, b: {b: 1}}, [0, 1, 2]);

verifyMin({a: 2, b: 'b'}, [3, 4, 5, 6, 7, 8]);
verifyMax({a: 2, b: 'b'}, [0, 1, 2]);

// Now a couple cases with an extra compound index.
t.drop();
t.ensureIndex({a: 1, b: -1, c: 1});
// The following documents are in order according to the index.
t.insert({_id: 0, a: 1, b: 'b', c: 1});
t.insert({_id: 1, a: 1, b: 'b', c: 2});
t.insert({_id: 2, a: 1, b: 'a', c: 1});
t.insert({_id: 3, a: 1, b: 'a', c: 2});
t.insert({_id: 4, a: 2, b: 'b', c: 1});
t.insert({_id: 5, a: 2, b: 'b', c: 2});
t.insert({_id: 6, a: 2, b: 'a', c: 1});
t.insert({_id: 7, a: 2, b: 'a', c: 2});

verifyMin({a: 1, b: 'a', c: 1}, [2, 3, 4, 5, 6, 7]);
verifyMin({a: 2, b: 'a', c: 2}, [7]);
verifyMax({a: 1, b: 'a', c: 1}, [0, 1]);
verifyMax({a: 2, b: 'a', c: 2}, [0, 1, 2, 3, 4, 5, 6]);

verifyMin({a: Infinity, b: 'a', c: 2}, []);
verifyMax({a: Infinity, b: 'a', c: 2}, [0, 1, 2, 3, 4, 5, 6, 7]);

verifyMin({a: -Infinity, b: 'a', c: 2}, [0, 1, 2, 3, 4, 5, 6, 7]);
verifyMax({a: -Infinity, b: 'a', c: 2}, []);

// 'a' > Infinity, actually.
verifyMin({a: 1, b: Infinity, c: 2}, [4, 5, 6, 7]);
verifyMax({a: 1, b: Infinity, c: 2}, [0, 1, 2, 3]);

// Also, 'a' > -Infinity.
verifyMin({a: 1, b: -Infinity, c: 2}, [4, 5, 6, 7]);
verifyMax({a: 1, b: -Infinity, c: 2}, [0, 1, 2, 3]);

verifyMin({a: 1, b: 'a', c: Infinity}, [4, 5, 6, 7]);
verifyMax({a: 1, b: 'a', c: Infinity}, [0, 1, 2, 3]);

verifyMin({a: 1, b: 'a', c: -Infinity}, [2, 3, 4, 5, 6, 7]);
verifyMax({a: 1, b: 'a', c: -Infinity}, [0, 1]);
