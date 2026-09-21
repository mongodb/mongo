/**
 * Regression test for SERVER-131802. The boolean expression simplifier incorrectly rewrote a
 * tautological $or that contains a predicate and its negation as separate single-literal disjuncts,
 * dropping documents from the result set.
 *
 * The filter (a & b!=1) | (a!=1) | (a & b) is always true, so it must match every document, but the
 * bad rewrite collapsed it to just {a: 1} and dropped every document with a != 1.
 */

const equalityColl = db[jsTestName() + "_equality"];
equalityColl.drop();
{
    const docs = [
        {_id: 0, a: 1, b: 1},
        {_id: 1, a: 1, b: 2},
        {_id: 2, a: 2, b: 1},
        {_id: 3, a: 2, b: 2},
    ];
    assert.commandWorked(equalityColl.insert(docs));

    const filter = {$or: [{a: 1, b: {$ne: 1}}, {a: {$ne: 1}}, {a: 1, b: 1}]};
    const results = equalityColl.find(filter).toArray();

    assert.eq(docs.length, results.length, "tautological $or must match every document", {results});
}

const rangeColl = db[jsTestName() + "_range"];
rangeColl.drop();
{
    const docs = [
        {_id: 0, a: 10, b: 1},
        {_id: 1, a: 10, b: 2},
        {_id: 2, a: 3, b: 1},
        {_id: 3, a: 3, b: 2},
    ];
    assert.commandWorked(rangeColl.insert(docs));

    // (a>5 & b!=1) | !(a>5) | (a>5 & b=1) is a tautology: every document satisfies at least one
    // of the three branches, so the filter must match everything.
    const filter = {$or: [{a: {$gt: 5}, b: {$ne: 1}}, {a: {$not: {$gt: 5}}}, {a: {$gt: 5}, b: 1}]};
    const results = rangeColl.find(filter).toArray();

    assert.eq(docs.length, results.length, "tautological $or must match every document", {results});
}
