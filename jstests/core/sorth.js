// Tests for the $in/sort/limit optimization combined with inequality bounds.  SERVER-5777

(function() {
    "use strict";

    var t = db.jstests_sorth;
    t.drop();

    // These can be set to modify the query run by the helper find().
    var _sort;
    var _limit;
    var _hint;

    /**
     * Generate a cursor using global parameters '_sort', '_hint', and '_limit'.
     */
    function find(query) {
        return t.find(query, {_id: 0}).sort(_sort).limit(_limit).hint(_hint);
    }

    /**
     * Returns true if the elements of 'expectedMatches' match element by element with
     * 'actualMatches', only considering the fields 'a' and 'b'.
     *
     * @param {Array} expectedMatches - expected results from a query.
     * @param {Array} actualMatches - the actual results from that query.
     */
    function resultsMatch(expectedMatches, actualMatches) {
        if (expectedMatches.length !== actualMatches.length) {
            return false;
        }

        for (var i = 0; i < expectedMatches.length; ++i) {
            if ((expectedMatches[i].a !== actualMatches[i].a) ||
                (expectedMatches[i].b !== actualMatches[i].b)) {
                return false;
            }
        }
        return true;
    }

    /**
     * Asserts that the given query returns results that are expected.
     *
     * @param {Object} options.query - the query to run.
     * @param {Array.<Object>} options.expectedQueryResults - the expected results from the query.
     * @param {Array.<Array>} [options.acceptableQueryResults=[options.expectedQueryResults]] - An
     * array of acceptable outcomes of the query. This can be used if there are multiple results
     * that are considered correct for the query.
     */
    function assertMatches(options) {
        const results = find(options.query).toArray();
        const acceptableQueryResults =
            options.acceptableQueryResults || [options.expectedQueryResults];
        assert.gte(acceptableQueryResults.length, 1);
        for (var i = 0; i < acceptableQueryResults.length; ++i) {
            const validResultSet = acceptableQueryResults[i];

            // All results should have the same number of results.
            assert.eq(validResultSet.length,
                      results.length,
                      "Expected " + results.length + " results from query " +
                          tojson(options.query) + " but found " + validResultSet.length);

            if (resultsMatch(validResultSet, results)) {
                return;
            }
        }
        throw new Error("Unexpected results for query " + tojson(options.query) + ": " +
                        tojson(results) + ", acceptable results were: " +
                        tojson(acceptableQueryResults));
    }

    /**
     * Reset data, index, and _sort and _hint globals.
     */
    function reset(sort, index) {
        t.drop();
        t.save({a: 1, b: 1});
        t.save({a: 1, b: 2});
        t.save({a: 1, b: 3});
        t.save({a: 2, b: 0});
        t.save({a: 2, b: 3});
        t.save({a: 2, b: 5});
        t.ensureIndex(index);
        _sort = sort;
        _hint = index;
    }

    function checkForwardDirection(options) {
        // All callers specify a sort that is prefixed by b, ascending.
        assert.eq(Object.keys(options.sort)[0], "b");
        assert.eq(options.sort.b, 1);

        // None of the callers specify a sort on "a".
        assert(!options.sort.hasOwnProperty("a"));

        reset(options.sort, options.index);

        _limit = -1;

        // Lower bound checks.
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 0}], query: {a: {$in: [1, 2]}, b: {$gte: 0}}});
        assertMatches(
            {expectedQueryResults: [{a: 1, b: 1}], query: {a: {$in: [1, 2]}, b: {$gt: 0}}});
        assertMatches(
            {expectedQueryResults: [{a: 1, b: 1}], query: {a: {$in: [1, 2]}, b: {$gte: 1}}});
        assertMatches(
            {expectedQueryResults: [{a: 1, b: 2}], query: {a: {$in: [1, 2]}, b: {$gt: 1}}});
        assertMatches(
            {expectedQueryResults: [{a: 1, b: 2}], query: {a: {$in: [1, 2]}, b: {$gte: 2}}});

        // Since we are sorting on the field "b", and the sort specification doesn't include the
        // field "a", any query that is expected to result in a document with a value of 3 for "b"
        // has two acceptable results, since there are two documents with a value of 3 for "b". The
        // same argument applies for all assertions below involving a result with a value of 3 for
        // the field "b".
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$gt: 2}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$gte: 3}}
        });
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$gt: 3}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$gte: 4}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$gt: 4}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$gte: 5}}});

        // Upper bound checks.
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 0}], query: {a: {$in: [1, 2]}, b: {$lte: 0}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 0}], query: {a: {$in: [1, 2]}, b: {$lt: 1}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 0}], query: {a: {$in: [1, 2]}, b: {$lte: 1}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 0}], query: {a: {$in: [1, 2]}, b: {$lt: 3}}});

        // Lower and upper bounds checks.
        assertMatches({
            expectedQueryResults: [{a: 2, b: 0}],
            query: {a: {$in: [1, 2]}, b: {$gte: 0, $lte: 0}}
        });
        assertMatches({
            expectedQueryResults: [{a: 2, b: 0}],
            query: {a: {$in: [1, 2]}, b: {$gte: 0, $lt: 1}}
        });
        assertMatches({
            expectedQueryResults: [{a: 2, b: 0}],
            query: {a: {$in: [1, 2]}, b: {$gte: 0, $lte: 1}}
        });
        assertMatches({
            expectedQueryResults: [{a: 1, b: 1}],
            query: {a: {$in: [1, 2]}, b: {$gt: 0, $lte: 1}}
        });
        assertMatches({
            expectedQueryResults: [{a: 1, b: 2}],
            query: {a: {$in: [1, 2]}, b: {$gte: 2, $lt: 3}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$gte: 2.5, $lte: 3}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$gt: 2.5, $lte: 3}}
        });

        // Limit is -2.
        _limit = -2;
        assertMatches({
            expectedQueryResults: [{a: 2, b: 0}, {a: 1, b: 1}],
            query: {a: {$in: [1, 2]}, b: {$gte: 0}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 2}, {a: 2, b: 3}], [{a: 1, b: 2}, {a: 1, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$gt: 1}}
        });
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$gt: 4}}});

        // With an additional document between the $in values.
        t.save({a: 1.5, b: 3});
        assertMatches({
            expectedQueryResults: [{a: 2, b: 0}, {a: 1, b: 1}],
            query: {a: {$in: [1, 2]}, b: {$gte: 0}}
        });
    }

    // Basic test with an index suffix order.
    checkForwardDirection({sort: {b: 1}, index: {a: 1, b: 1}});
    // With an additional index field.
    checkForwardDirection({sort: {b: 1}, index: {a: 1, b: 1, c: 1}});
    // With an additional reverse direction index field.
    checkForwardDirection({sort: {b: 1}, index: {a: 1, b: 1, c: -1}});
    // With an additional ordered index field.
    checkForwardDirection({sort: {b: 1, c: 1}, index: {a: 1, b: 1, c: 1}});
    // With an additional reverse direction ordered index field.
    checkForwardDirection({sort: {b: 1, c: -1}, index: {a: 1, b: 1, c: -1}});

    function checkReverseDirection(options) {
        // All callers specify a sort that is prefixed by "b", descending.
        assert.eq(Object.keys(options.sort)[0], "b");
        assert.eq(options.sort.b, -1);
        // None of the callers specify a sort on "a".
        assert(!options.sort.hasOwnProperty("a"));

        reset(options.sort, options.index);
        _limit = -1;

        // For matching documents, highest value of 'b' is 5.
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$gte: 0}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$gte: 5}}});
        assertMatches(
            {expectedQueryResults: [{a: 2, b: 5}], query: {a: {$in: [1, 2]}, b: {$lte: 5}}});
        assertMatches({
            expectedQueryResults: [{a: 2, b: 5}],
            query: {a: {$in: [1, 2]}, b: {$lte: 5, $gte: 5}}
        });

        // For matching documents, highest value of 'b' is 2.
        assertMatches(
            {expectedQueryResults: [{a: 1, b: 2}], query: {a: {$in: [1, 2]}, b: {$lt: 3}}});
        assertMatches(
            {expectedQueryResults: [{a: 1, b: 2}], query: {a: {$in: [1, 2]}, b: {$lt: 3, $gt: 1}}});

        // For matching documents, highest value of 'b' is 1.
        assertMatches({
            expectedQueryResults: [{a: 1, b: 1}],
            query: {a: {$in: [1, 2]}, b: {$lt: 2, $gte: 1}}
        });

        // These queries expect 3 as the highest value of 'b' among matching documents, but there
        // are two documents with a value of 3 for the field 'b'. Either document is acceptable,
        // since there is no sort order on any other existing fields.
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$lt: 5}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$lt: 3.1}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$lt: 3.5}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$lte: 3}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$lt: 3.5, $gte: 3}}
        });
        assertMatches({
            acceptableQueryResults: [[{a: 1, b: 3}], [{a: 2, b: 3}]],
            query: {a: {$in: [1, 2]}, b: {$lte: 3, $gt: 0}}
        });
    }

    // With a descending order index.
    checkReverseDirection({sort: {b: -1}, index: {a: 1, b: -1}});
    checkReverseDirection({sort: {b: -1}, index: {a: 1, b: -1, c: 1}});
    checkReverseDirection({sort: {b: -1}, index: {a: 1, b: -1, c: -1}});
    checkReverseDirection({sort: {b: -1, c: 1}, index: {a: 1, b: -1, c: 1}});
    checkReverseDirection({sort: {b: -1, c: -1}, index: {a: 1, b: -1, c: -1}});
}());
