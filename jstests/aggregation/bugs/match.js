// Check $match pipeline stage.
// - Filtering behavior equivalent to a mongo query.
// - $where and geo operators are not allowed
(function() {
    "use strict";

    load('jstests/aggregation/extras/utils.js');

    const coll = db.jstests_aggregation_match;
    coll.drop();

    const identityProjection = {_id: '$_id', a: '$a'};

    /** Assert that an aggregation generated the expected error. */
    function assertError(expectedCode, matchSpec) {
        const matchStage = {$match: matchSpec};
        // Check where matching is folded in to DocumentSourceCursor.
        assertErrorCode(coll, [matchStage], expectedCode);
        // Check where matching is not folded in to DocumentSourceCursor.
        assertErrorCode(coll, [{$project: identityProjection}, matchStage], expectedCode);
    }

    /** Assert that the contents of two arrays are equal, ignoring element ordering. */
    function assertEqualResultsUnordered(one, two) {
        let oneStr = one.map(function(x) {
            return tojson(x);
        });
        let twoStr = two.map(function(x) {
            return tojson(x);
        });
        oneStr.sort();
        twoStr.sort();
        assert.eq(oneStr, twoStr);
    }

    /** Assert that an aggregation result is as expected. */
    function assertResults(expectedResults, matchSpec) {
        const findResults = coll.find(matchSpec).toArray();
        if (expectedResults) {
            assertEqualResultsUnordered(expectedResults, findResults);
        }
        const matchStage = {$match: matchSpec};
        // Check where matching is folded in to DocumentSourceCursor.
        assertEqualResultsUnordered(findResults, coll.aggregate(matchStage).toArray());
        // Check where matching is not folded in to DocumentSourceCursor.
        assertEqualResultsUnordered(
            findResults, coll.aggregate({$project: identityProjection}, matchStage).toArray());
    }

    // Invalid matcher syntax.
    assertError(2, {a: {$mod: [0 /* invalid */, 0]}});

    // $where not allowed.
    assertError(ErrorCodes.BadValue, {$where: 'true'});

    // Geo not allowed.
    assertError(ErrorCodes.BadValue, {$match: {a: {$near: [0, 0]}}});

    function checkMatchResults(indexed) {
        // No results.
        coll.remove({});
        assertResults([], {});

        assert.writeOK(coll.insert({_id: 0, a: 1}));
        assert.writeOK(coll.insert({_id: 1, a: 2}));
        assert.writeOK(coll.insert({_id: 2, a: 3}));

        // Empty query.
        assertResults([{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}], {});

        // Simple queries.
        assertResults([{_id: 0, a: 1}], {a: 1});
        assertResults([{_id: 1, a: 2}], {a: 2});
        assertResults([{_id: 1, a: 2}, {_id: 2, a: 3}], {a: {$gt: 1}});
        assertResults([{_id: 0, a: 1}, {_id: 1, a: 2}], {a: {$lte: 2}});
        assertResults([{_id: 0, a: 1}, {_id: 2, a: 3}], {a: {$in: [1, 3]}});

        // Regular expression.
        coll.remove({});
        assert.writeOK(coll.insert({_id: 0, a: 'x'}));
        assert.writeOK(coll.insert({_id: 1, a: 'yx'}));
        assertResults([{_id: 0, a: 'x'}], {a: /^x/});
        assertResults([{_id: 0, a: 'x'}, {_id: 1, a: 'yx'}], {a: /x/});

        // Dotted field.
        coll.remove({});
        assert.writeOK(coll.insert({_id: 0, a: {b: 4}}));
        assert.writeOK(coll.insert({_id: 1, a: 2}));
        assertResults([{_id: 0, a: {b: 4}}], {'a.b': 4});

        // Value within an array.
        coll.remove({});
        assert.writeOK(coll.insert({_id: 0, a: [1, 2, 3]}));
        assert.writeOK(coll.insert({_id: 1, a: [2, 2, 3]}));
        assert.writeOK(coll.insert({_id: 2, a: [2, 2, 2]}));
        assertResults([{_id: 0, a: [1, 2, 3]}, {_id: 1, a: [2, 2, 3]}], {a: 3});

        // Missing, null, $exists matching.
        coll.remove({});
        assert.writeOK(coll.insert({_id: 0}));
        assert.writeOK(coll.insert({_id: 1, a: null}));
        assert.writeOK(coll.insert({_id: 3, a: 0}));
        assertResults([{_id: 0}, {_id: 1, a: null}], {a: null});
        assertResults(null, {a: {$exists: true}});
        assertResults(null, {a: {$exists: false}});

        // $elemMatch
        coll.remove({});
        assert.writeOK(coll.insert({_id: 0, a: [1, 2]}));
        assert.writeOK(coll.insert({_id: 1, a: [1, 2, 3]}));
        assertResults([{_id: 1, a: [1, 2, 3]}], {a: {$elemMatch: {$gt: 1, $mod: [2, 1]}}});

        coll.remove({});
        assert.writeOK(coll.insert({_id: 0, a: [{b: 1}, {c: 2}]}));
        assert.writeOK(coll.insert({_id: 1, a: [{b: 1, c: 2}]}));
        assertResults([{_id: 1, a: [{b: 1, c: 2}]}], {a: {$elemMatch: {b: 1, c: 2}}});

        // $size
        coll.remove({});
        assert.writeOK(coll.insert({}));
        assert.writeOK(coll.insert({a: null}));
        assert.writeOK(coll.insert({a: []}));
        assert.writeOK(coll.insert({a: [1]}));
        assert.writeOK(coll.insert({a: [1, 2]}));
        assertResults(null, {a: {$size: 0}});
        assertResults(null, {a: {$size: 1}});
        assertResults(null, {a: {$size: 2}});

        // $type
        coll.remove({});
        assert.writeOK(coll.insert({}));
        assert.writeOK(coll.insert({a: null}));
        assert.writeOK(coll.insert({a: NumberInt(1)}));
        assert.writeOK(coll.insert({a: NumberLong(2)}));
        assert.writeOK(coll.insert({a: 66.6}));
        assert.writeOK(coll.insert({a: 'abc'}));
        assert.writeOK(coll.insert({a: /xyz/}));
        assert.writeOK(coll.insert({a: {q: 1}}));
        assert.writeOK(coll.insert({a: true}));
        assert.writeOK(coll.insert({a: new Date()}));
        assert.writeOK(coll.insert({a: new ObjectId()}));
        for (let type = 1; type <= 18; ++type) {
            assertResults(null, {a: {$type: type}});
        }

        coll.remove({});
        assert.writeOK(coll.insert({_id: 0, a: 1}));
        assert.writeOK(coll.insert({_id: 1, a: 2}));
        assert.writeOK(coll.insert({_id: 2, a: 3}));

        // $and
        assertResults([{_id: 1, a: 2}], {$and: [{a: 2}, {_id: 1}]});
        assertResults([], {$and: [{a: 1}, {_id: 1}]});
        assertResults([{_id: 1, a: 2}, {_id: 2, a: 3}],
                      {$and: [{$or: [{_id: 1}, {a: 3}]}, {$or: [{_id: 2}, {a: 2}]}]});

        // $or
        assertResults([{_id: 0, a: 1}, {_id: 2, a: 3}], {$or: [{_id: 0}, {a: 3}]});
    }

    checkMatchResults(false);
    coll.createIndex({a: 1});
    checkMatchResults(true);
    coll.createIndex({'a.b': 1});
    coll.createIndex({'a.c': 1});
    checkMatchResults(true);
})();
