// Check $match pipeline stage.
// - Filtering behavior equivalent to a mongo query.
// - $where and geo operators are not allowed
load('jstests/aggregation/extras/utils.js');

t = db.jstests_aggregation_match;
t.drop();

identityProjection = {
    _id: '$_id',
    a: '$a'
};

/** Assert that an aggregation generated the expected error. */
function assertError(expectedCode, matchSpec) {
    matchStage = {$match: matchSpec};
    // Check where matching is folded in to DocumentSourceCursor.
    assertErrorCode(t, [matchStage], expectedCode);
    // Check where matching is not folded in to DocumentSourceCursor.
    assertErrorCode(t, [{$project: identityProjection}, matchStage], expectedCode);
}

/** Assert that the contents of two arrays are equal, ignoring element ordering. */
function assertEqualResultsUnordered(one, two) {
    oneStr = one.map(function(x) {
        return tojson(x);
    });
    twoStr = two.map(function(x) {
        return tojson(x);
    });
    oneStr.sort();
    twoStr.sort();
    assert.eq(oneStr, twoStr);
}

/** Assert that an aggregation result is as expected. */
function assertResults(expectedResults, matchSpec) {
    findResults = t.find(matchSpec).toArray();
    if (expectedResults) {
        assertEqualResultsUnordered(expectedResults, findResults);
    }
    matchStage = {$match: matchSpec};
    // Check where matching is folded in to DocumentSourceCursor.
    assertEqualResultsUnordered(findResults, t.aggregate(matchStage).toArray());
    // Check where matching is not folded in to DocumentSourceCursor.
    assertEqualResultsUnordered(findResults,
                                t.aggregate({$project: identityProjection}, matchStage).toArray());
}

// Invalid matcher syntax.
assertError(2, {a: {$mod: [0 /* invalid */, 0]}});

// $where not allowed.
assertError(16395, {$where: 'true'});

// Geo not allowed.
assertError(16424, {$match: {a: {$near: [0, 0]}}});

// Update modifier not allowed.
if (0) {  // SERVER-6650
    assertError(0, {a: 1, $inc: {b: 1}});
}

// Aggregation expression not allowed.
if (0) {  // SERVER-6650
    assertError(0, {a: 1, b: {$gt: {$add: [1, 1]}}});
}

function checkMatchResults(indexed) {
    // No results.
    t.remove({});
    assertResults([], {});

    t.save({_id: 0, a: 1});
    t.save({_id: 1, a: 2});
    t.save({_id: 2, a: 3});

    // Empty query.
    assertResults([{_id: 0, a: 1}, {_id: 1, a: 2}, {_id: 2, a: 3}], {});

    // Simple queries.
    assertResults([{_id: 0, a: 1}], {a: 1});
    assertResults([{_id: 1, a: 2}], {a: 2});
    assertResults([{_id: 1, a: 2}, {_id: 2, a: 3}], {a: {$gt: 1}});
    assertResults([{_id: 0, a: 1}, {_id: 1, a: 2}], {a: {$lte: 2}});
    assertResults([{_id: 0, a: 1}, {_id: 2, a: 3}], {a: {$in: [1, 3]}});

    // Regular expression.
    t.remove({});
    t.save({_id: 0, a: 'x'});
    t.save({_id: 1, a: 'yx'});
    assertResults([{_id: 0, a: 'x'}], {a: /^x/});
    assertResults([{_id: 0, a: 'x'}, {_id: 1, a: 'yx'}], {a: /x/});

    // Dotted field.
    t.remove({});
    t.save({_id: 0, a: {b: 4}});
    t.save({_id: 1, a: 2});
    assertResults([{_id: 0, a: {b: 4}}], {'a.b': 4});

    // Value within an array.
    t.remove({});
    t.save({_id: 0, a: [1, 2, 3]});
    t.save({_id: 1, a: [2, 2, 3]});
    t.save({_id: 2, a: [2, 2, 2]});
    assertResults([{_id: 0, a: [1, 2, 3]}, {_id: 1, a: [2, 2, 3]}], {a: 3});

    // Missing, null, $exists matching.
    t.remove({});
    t.save({_id: 0});
    t.save({_id: 1, a: null});
    if (0) {  // SERVER-6571
        t.save({_id: 2, a: undefined});
    }
    t.save({_id: 3, a: 0});
    assertResults([{_id: 0}, {_id: 1, a: null}], {a: null});
    assertResults(null, {a: {$exists: true}});
    assertResults(null, {a: {$exists: false}});

    // $elemMatch
    t.remove({});
    t.save({_id: 0, a: [1, 2]});
    t.save({_id: 1, a: [1, 2, 3]});
    assertResults([{_id: 1, a: [1, 2, 3]}], {a: {$elemMatch: {$gt: 1, $mod: [2, 1]}}});

    t.remove({});
    t.save({_id: 0, a: [{b: 1}, {c: 2}]});
    t.save({_id: 1, a: [{b: 1, c: 2}]});
    assertResults([{_id: 1, a: [{b: 1, c: 2}]}], {a: {$elemMatch: {b: 1, c: 2}}});

    // $size
    t.remove({});
    t.save({});
    t.save({a: null});
    t.save({a: []});
    t.save({a: [1]});
    t.save({a: [1, 2]});
    assertResults(null, {a: {$size: 0}});
    assertResults(null, {a: {$size: 1}});
    assertResults(null, {a: {$size: 2}});

    // $type
    t.remove({});
    t.save({});
    t.save({a: null});
    if (0) {  // SERVER-6571
        t.save({a: undefined});
    }
    t.save({a: NumberInt(1)});
    t.save({a: NumberLong(2)});
    t.save({a: 66.6});
    t.save({a: 'abc'});
    t.save({a: /xyz/});
    t.save({a: {q: 1}});
    t.save({a: true});
    t.save({a: new Date()});
    t.save({a: new ObjectId()});
    for (type = 1; type <= 18; ++type) {
        assertResults(null, {a: {$type: type}});
    }

    // $atomic does not affect results.
    t.remove({});
    t.save({_id: 0, a: 1});
    t.save({_id: 1, a: 2});
    t.save({_id: 2, a: 3});
    assertResults([{_id: 0, a: 1}], {a: 1, $atomic: true});
    assertResults([{_id: 1, a: 2}], {a: 2, $atomic: true});
    assertResults([{_id: 1, a: 2}, {_id: 2, a: 3}], {a: {$gt: 1}, $atomic: true});
    assertResults([{_id: 0, a: 1}, {_id: 1, a: 2}], {a: {$lte: 2}, $atomic: true});
    assertResults([{_id: 0, a: 1}, {_id: 2, a: 3}], {a: {$in: [1, 3]}, $atomic: true});

    // $and
    assertResults([{_id: 1, a: 2}], {$and: [{a: 2}, {_id: 1}]});
    assertResults([], {$and: [{a: 1}, {_id: 1}]});
    assertResults([{_id: 1, a: 2}, {_id: 2, a: 3}],
                  {$and: [{$or: [{_id: 1}, {a: 3}]}, {$or: [{_id: 2}, {a: 2}]}]});

    // $or
    assertResults([{_id: 0, a: 1}, {_id: 2, a: 3}], {$or: [{_id: 0}, {a: 3}]});
}

checkMatchResults(false);
t.ensureIndex({a: 1});
checkMatchResults(true);
t.ensureIndex({'a.b': 1});
t.ensureIndex({'a.c': 1});
checkMatchResults(true);
