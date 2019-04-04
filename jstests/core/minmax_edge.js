/*
 * Test that .min() and .max() queries properly handle the edge cases with NaN and Infinity.
 * Other edge cases are covered by C++ unit tests.
 */
(function() {
    const t = db.minmax_edge;

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
    function verifyMin(minDoc, idx, expectedIds) {
        verifyResultIds(t.find().min(minDoc).hint(idx).toArray(), expectedIds);
    }

    function verifyMax(minDoc, idx, expectedIds) {
        verifyResultIds(t.find().max(minDoc).hint(idx).toArray(), expectedIds);
    }

    // Basic ascending index.
    reset(t);
    let indexSpec = {a: 1};
    assert.commandWorked(t.createIndex(indexSpec));

    verifyMin({a: Infinity}, indexSpec, []);
    verifyMax({a: Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: -Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: -Infinity}, indexSpec, []);

    // NaN < all ints.
    verifyMin({a: NaN}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: NaN}, indexSpec, []);

    // {a: 1} > all ints.
    verifyMin({a: {a: 1}}, indexSpec, []);
    verifyMax({a: {a: 1}}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    // 'a' > all ints.
    verifyMin({a: 'a'}, indexSpec, []);
    verifyMax({a: 'a'}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    // Now with a compound index.
    reset(t);
    indexSpec = {a: 1, b: -1};

    assert.commandWorked(t.createIndex(indexSpec));

    // Same as single-key index assertions, with b field present.
    verifyMin({a: NaN, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: NaN, b: 1}, indexSpec, []);

    verifyMin({a: Infinity, b: 1}, indexSpec, []);
    verifyMax({a: Infinity, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: -Infinity, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: -Infinity, b: 1}, indexSpec, []);

    verifyMin({a: {a: 1}, b: 1}, indexSpec, []);
    verifyMax({a: {a: 1}, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: 'a', b: 1}, indexSpec, []);
    verifyMax({a: 'a', b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    // Edge cases on b values
    verifyMin({a: 1, b: Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMin({a: 2, b: Infinity}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMin({a: 3, b: Infinity}, indexSpec, [6, 7, 8]);
    verifyMax({a: 1, b: Infinity}, indexSpec, []);
    verifyMax({a: 2, b: Infinity}, indexSpec, [0, 1, 2]);
    verifyMax({a: 3, b: Infinity}, indexSpec, [0, 1, 2, 3, 4, 5]);

    verifyMin({a: 1, b: -Infinity}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMin({a: 2, b: -Infinity}, indexSpec, [6, 7, 8]);
    verifyMin({a: 3, b: -Infinity}, indexSpec, []);
    verifyMax({a: 1, b: -Infinity}, indexSpec, [0, 1, 2]);
    verifyMax({a: 2, b: -Infinity}, indexSpec, [0, 1, 2, 3, 4, 5]);
    verifyMax({a: 3, b: -Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: 2, b: NaN}, indexSpec, [6, 7, 8]);
    verifyMax({a: 2, b: NaN}, indexSpec, [0, 1, 2, 3, 4, 5]);

    verifyMin({a: 2, b: {b: 1}}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMax({a: 2, b: {b: 1}}, indexSpec, [0, 1, 2]);

    verifyMin({a: 2, b: 'b'}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMax({a: 2, b: 'b'}, indexSpec, [0, 1, 2]);

    // Test descending index.
    reset(t);
    indexSpec = {a: -1};
    assert.commandWorked(t.createIndex(indexSpec));

    verifyMin({a: NaN}, indexSpec, []);
    verifyMax({a: NaN}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: Infinity}, indexSpec, []);

    verifyMin({a: -Infinity}, indexSpec, []);
    verifyMax({a: -Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: {a: 1}}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: {a: 1}}, indexSpec, []);

    verifyMin({a: 'a'}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: 'a'}, indexSpec, []);

    // Now with a compound index.
    reset(t);
    indexSpec = {a: -1, b: -1};
    assert.commandWorked(t.createIndex(indexSpec));

    // Same as single-key index assertions, with b field present.
    verifyMin({a: NaN, b: 1}, indexSpec, []);
    verifyMax({a: NaN, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: Infinity, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: Infinity, b: 1}, indexSpec, []);

    verifyMin({a: -Infinity, b: 1}, indexSpec, []);
    verifyMax({a: -Infinity, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);

    verifyMin({a: {a: 1}, b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: {a: 1}, b: 1}, indexSpec, []);

    verifyMin({a: 'a', b: 1}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: 'a', b: 1}, indexSpec, []);

    // Edge cases on b values.
    verifyMin({a: 1, b: Infinity}, indexSpec, [0, 1, 2]);
    verifyMin({a: 2, b: Infinity}, indexSpec, [0, 1, 2, 3, 4, 5]);
    verifyMin({a: 3, b: Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: 1, b: Infinity}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMax({a: 2, b: Infinity}, indexSpec, [6, 7, 8]);
    verifyMax({a: 3, b: Infinity}, indexSpec, []);

    verifyMin({a: 1, b: -Infinity}, indexSpec, []);
    verifyMin({a: 2, b: -Infinity}, indexSpec, [0, 1, 2]);
    verifyMin({a: 3, b: -Infinity}, indexSpec, [0, 1, 2, 3, 4, 5]);
    verifyMax({a: 1, b: -Infinity}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7, 8]);
    verifyMax({a: 2, b: -Infinity}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMax({a: 3, b: -Infinity}, indexSpec, [6, 7, 8]);

    verifyMin({a: 2, b: NaN}, indexSpec, [0, 1, 2]);
    verifyMax({a: 2, b: NaN}, indexSpec, [3, 4, 5, 6, 7, 8]);

    verifyMin({a: 2, b: {b: 1}}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMax({a: 2, b: {b: 1}}, indexSpec, [0, 1, 2]);

    verifyMin({a: 2, b: 'b'}, indexSpec, [3, 4, 5, 6, 7, 8]);
    verifyMax({a: 2, b: 'b'}, indexSpec, [0, 1, 2]);

    // Now a couple cases with an extra compound index.
    t.drop();
    indexSpec = {a: 1, b: -1, c: 1};
    assert.commandWorked(t.createIndex(indexSpec));
    // The following documents are in order according to the index.
    t.insert({_id: 0, a: 1, b: 'b', c: 1});
    t.insert({_id: 1, a: 1, b: 'b', c: 2});
    t.insert({_id: 2, a: 1, b: 'a', c: 1});
    t.insert({_id: 3, a: 1, b: 'a', c: 2});
    t.insert({_id: 4, a: 2, b: 'b', c: 1});
    t.insert({_id: 5, a: 2, b: 'b', c: 2});
    t.insert({_id: 6, a: 2, b: 'a', c: 1});
    t.insert({_id: 7, a: 2, b: 'a', c: 2});

    verifyMin({a: 1, b: 'a', c: 1}, indexSpec, [2, 3, 4, 5, 6, 7]);
    verifyMin({a: 2, b: 'a', c: 2}, indexSpec, [7]);
    verifyMax({a: 1, b: 'a', c: 1}, indexSpec, [0, 1]);
    verifyMax({a: 2, b: 'a', c: 2}, indexSpec, [0, 1, 2, 3, 4, 5, 6]);

    verifyMin({a: Infinity, b: 'a', c: 2}, indexSpec, []);
    verifyMax({a: Infinity, b: 'a', c: 2}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7]);

    verifyMin({a: -Infinity, b: 'a', c: 2}, indexSpec, [0, 1, 2, 3, 4, 5, 6, 7]);
    verifyMax({a: -Infinity, b: 'a', c: 2}, indexSpec, []);

    // 'a' > Infinity, actually.
    verifyMin({a: 1, b: Infinity, c: 2}, indexSpec, [4, 5, 6, 7]);
    verifyMax({a: 1, b: Infinity, c: 2}, indexSpec, [0, 1, 2, 3]);

    // Also, 'a' > -Infinity.
    verifyMin({a: 1, b: -Infinity, c: 2}, indexSpec, [4, 5, 6, 7]);
    verifyMax({a: 1, b: -Infinity, c: 2}, indexSpec, [0, 1, 2, 3]);

    verifyMin({a: 1, b: 'a', c: Infinity}, indexSpec, [4, 5, 6, 7]);
    verifyMax({a: 1, b: 'a', c: Infinity}, indexSpec, [0, 1, 2, 3]);

    verifyMin({a: 1, b: 'a', c: -Infinity}, indexSpec, [2, 3, 4, 5, 6, 7]);
    verifyMax({a: 1, b: 'a', c: -Infinity}, indexSpec, [0, 1]);
})();
