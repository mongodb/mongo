/**
 * This test ensures that queries using simple ranges handle bound inclusion properly.
 * @tags: [
 *   assumes_balancer_off
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fixture_helpers.js');      // For FixtureHelpers.
load('jstests/aggregation/extras/utils.js');  // For resultsEq.

var coll = db.query_bound_inclusion;
coll.drop();
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 2, b: 2}));
assert.commandWorked(coll.insert({a: 3, b: 3}));

assert.commandWorked(coll.createIndex({a: 1}));

var res = coll.find().sort({a: 1}).toArray();
assert.eq(res.length, 3);
assert.eq(res[0].a, 1);
assert.eq(res[1].a, 2);
assert.eq(res[2].a, 3);

res = coll.find().sort({a: -1}).toArray();
assert.eq(res.length, 3);
assert.eq(res[0].a, 3);
assert.eq(res[1].a, 2);
assert.eq(res[2].a, 1);

res = coll.find().min({a: 1}).max({a: 3}).hint({a: 1}).toArray();
assert.eq(res.length, 2);
if (FixtureHelpers.numberOfShardsForCollection(coll) === 1) {
    assert.eq(res[0].a, 1);
    assert.eq(res[1].a, 2);
} else {
    // With more than one shard, we cannot assume the results will come back in order, since we
    // did not request a sort.
    assert(resultsEq(res.map((result) => result.a), [1, 2]));
}

res = coll.find().min({a: 1}).max({a: 3}).sort({a: -1}).hint({a: 1}).toArray();
assert.eq(res.length, 2);
assert.eq(res[0].a, 2);
assert.eq(res[1].a, 1);

assert.commandWorked(coll.createIndex({b: -1}));

res = coll.find().sort({b: -1}).toArray();
assert.eq(res.length, 3);
assert.eq(res[0].b, 3);
assert.eq(res[1].b, 2);
assert.eq(res[2].b, 1);

res = coll.find().sort({b: 1}).toArray();
assert.eq(res.length, 3);
assert.eq(res[0].b, 1);
assert.eq(res[1].b, 2);
assert.eq(res[2].b, 3);

res = coll.find().min({b: 3}).max({b: 1}).hint({b: -1}).toArray();
assert.eq(res.length, 2);
if (FixtureHelpers.numberOfShardsForCollection(coll) === 1) {
    assert.eq(res[0].b, 3);
    assert.eq(res[1].b, 2);
} else {
    // With more than one shard, we cannot assume the results will come back in order, since we
    // did not request a sort.
    assert(resultsEq(res.map((result) => result.b), [3, 2]));
}

res = coll.find().min({b: 3}).max({b: 1}).sort({b: 1}).hint({b: -1}).toArray();
assert.eq(res.length, 2);
assert.eq(res[0].b, 2);
assert.eq(res[1].b, 3);

// Bug SERVER-54552.
const testBoundsInclusivity = (indexDirection) => {
    coll.drop();
    coll.createIndex({i: indexDirection});
    coll.insertMany([{i: 'a'}, {i: 'b'}, {i: 'c'}, {i: 'd'}, {i: 'd'}, {i: 'e'}]);

    // Test for all the posible ranges.
    [['$gte', '$lte', 4], ['$gt', '$lte', 3], ['$gte', '$lt', 2], ['$gt', '$lt', 1]].forEach(
        ([gt, lt, count]) => {
            let query = {i: {}};
            query['i'][gt] = 'b';
            query['i'][lt] = 'd';

            assert.eq(count, coll.find(query).count(), {query, indexDirection});

            // Test with forward and backward sorts.
            assert.eq(count,
                      coll.find(query).sort({i: indexDirection}).count(),
                      {query, sort: 1, indexDirection});
            assert.eq(
                count, coll.find(query).sort({i: -1}).count(), {query, sort: -1, indexDirection});
        });
};
testBoundsInclusivity(1);
testBoundsInclusivity(-1);

// Tests inclusivity correctness with all sort directions on an index with a given direction
// for range queries that can use a 2 property compound index.
const testBoundsInclusivityCompound = (lIndexDirection, nIndexDirection) => {
    coll.drop();
    coll.createIndex({l: lIndexDirection, n: nIndexDirection});
    ['a', 'b', 'c', 'd', 'e'].forEach(
        (l) => [1, 2, 3, 4, 5].forEach((n) => coll.insertOne({l, n})));

    // The eq variants of the operators add one more document to the result.
    const opExtra = {'$lt': 0, '$lte': 1, '$gt': 0, '$gte': 1};
    const lts = ['$lt', '$lte'];
    const gts = ['$gt', '$gte'];

    // Iterate through all posible combinations of operators.
    lts.forEach((llt) => gts.forEach((lgt) => {
        lts.forEach((nlt) => gts.forEach((ngt) => {
            let query = {l: {}, n: {}};
            query['l'][llt] = 'd';
            query['l'][lgt] = 'b';
            query['n'][nlt] = 4;
            query['n'][ngt] = 2;

            // Calculate the count of documents based on operators used.
            const count = (1 + opExtra[llt] + opExtra[lgt]) * (1 + opExtra[nlt] + opExtra[ngt]);
            assert.eq(count, coll.find(query).count(), {query, lIndexDirection, nIndexDirection});

            // Also check with all posible sort combinations.
            const withSort = (lSort, nSort) => {
                assert.eq(count,
                          coll.find(query).sort({l: lSort, n: nSort}).count(),
                          {query, lSort, nSort, lIndexDirection, nIndexDirection});
            };
            withSort(1, 1);
            withSort(-1, 1);
            withSort(1, -1);
            withSort(-1, -1);
        }));
    }));
};
testBoundsInclusivityCompound(1, 1);
testBoundsInclusivityCompound(-1, 1);
testBoundsInclusivityCompound(1, -1);
testBoundsInclusivityCompound(-1, -1);
})();
