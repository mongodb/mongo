/**
 * Equivalence tests for $matchTopN extension and the stages it desugars into ({$match, $sort, $limit}).
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

function matchTopN(spec) {
    return [{$matchTopN: spec}];
}

function classic(spec) {
    return [{$match: spec.filter || {}}, {$sort: spec.sort}, {$limit: spec.limit}];
}

function assertEquiv(spec, msg) {
    const expected = coll.aggregate(classic(spec)).toArray();
    const actual = coll.aggregate(matchTopN(spec)).toArray();
    assert.eq(actual, expected, msg);
}

coll.insertMany([
    {_id: 1, a: "A", x: 5, y: 1},
    {_id: 2, a: "B", x: 3, y: 2},
    {_id: 3, a: "A", x: 9, y: 3},
    {_id: 4, a: "B", x: 7, y: 4},
    {_id: 5, a: "A", x: 1, y: 5},
]);

assertEquiv({filter: {}, sort: {x: -1}, limit: 3}, "descending by x");
assertEquiv({filter: {}, sort: {x: 1}, limit: 2}, "ascending by x");
assertEquiv({filter: {a: "A"}, sort: {x: -1}, limit: 10}, "filter + descending by x");
assertEquiv({filter: {}, sort: {a: 1, x: -1}, limit: 5}, "compound sort");
assertEquiv({filter: {}, sort: {x: -1}, limit: 99}, "limit larger than match set");
assertEquiv({filter: {}, sort: {x: -1}, limit: 1}, "limit 1");
assertEquiv({filter: {a: "Z"}, sort: {x: -1}, limit: 3}, "filter matches no documents");

{
    const spec = {filter: {}, sort: {x: -1}, limit: 5};
    const expected = coll.aggregate(classic(spec)).toArray();
    const cur = coll.aggregate(matchTopN(spec), {cursor: {batchSize: 2}});
    const paged = [];
    while (cur.hasNext()) {
        paged.push(cur.next());
    }
    assert.eq(paged, expected, "batching should not change results");
}
