/**
 * Tests that an $elemMatch-$or query is evaluated correctly. Designed to reproduce SERVER-33005 and
 * SERVER-38164.
 *  @tags: [
 *    requires_getmore,
 *  ]
 */
const coll = db.elemmatch_or_pushdown;
coll.drop();

// Confirms that 'query' returns 'expectedIds' both when answered by the index 'keyPattern' and when
// answered by a COLLSCAN.
function assertSameResultsWithIndexAndCollscan(keyPattern, query, expectedIds, msg) {
    const expected = expectedIds.map((id) => ({_id: id}));
    for (let hint of [keyPattern, {$natural: 1}]) {
        assert.eq(coll.find(query, {_id: 1}).sort({_id: 1}).hint(hint).toArray(), expected, msg, {
            hint,
        });
    }
}

assert.commandWorked(coll.insert({_id: 0, a: 1, b: [{c: 4}]}));
assert.commandWorked(coll.insert({_id: 1, a: 2, b: [{c: 4}]}));
assert.commandWorked(coll.insert({_id: 2, a: 2, b: [{c: 5}]}));
assert.commandWorked(coll.insert({_id: 3, a: 1, b: [{c: 5}]}));
assert.commandWorked(coll.insert({_id: 4, a: 1, b: [{c: 6}]}));
assert.commandWorked(coll.insert({_id: 5, a: 1, b: [{c: 7}]}));
assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));

assert.eq(
    coll
        .find({a: 1, b: {$elemMatch: {$or: [{c: 4}, {c: 5}]}}})
        .sort({_id: 1})
        .toArray(),
    [
        {_id: 0, a: 1, b: [{c: 4}]},
        {_id: 3, a: 1, b: [{c: 5}]},
    ],
);
assert.eq(
    coll
        .find({a: 1, $or: [{a: 2}, {b: {$elemMatch: {$or: [{c: 4}, {c: 5}]}}}]})
        .sort({_id: 1})
        .toArray(),
    [
        {_id: 0, a: 1, b: [{c: 4}]},
        {_id: 3, a: 1, b: [{c: 5}]},
    ],
);

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 5, b: [{c: [{f: 8}], d: 6}]}));
assert.commandWorked(coll.insert({_id: 1, a: 4, b: [{c: [{f: 8}], d: 6}]}));
assert.commandWorked(coll.insert({_id: 2, a: 5, b: [{c: [{f: 8}], d: 7}]}));
assert.commandWorked(coll.insert({_id: 3, a: 4, b: [{c: [{f: 9}], d: 6}]}));
assert.commandWorked(coll.insert({_id: 4, a: 5, b: [{c: [{f: 8}], e: 7}]}));
assert.commandWorked(coll.insert({_id: 5, a: 4, b: [{c: [{f: 8}], e: 7}]}));
assert.commandWorked(coll.insert({_id: 6, a: 5, b: [{c: [{f: 8}], e: 8}]}));
assert.commandWorked(coll.insert({_id: 7, a: 5, b: [{c: [{f: 9}], e: 7}]}));
assert.commandWorked(coll.createIndex({"b.d": 1, "b.c.f": 1}));
assert.commandWorked(coll.createIndex({"b.e": 1, "b.c.f": 1}));

assert.eq(
    coll
        .find({a: 5, b: {$elemMatch: {c: {$elemMatch: {f: 8}}, $or: [{d: 6}, {e: 7}]}}})
        .sort({_id: 1})
        .toArray(),
    [
        {_id: 0, a: 5, b: [{c: [{f: 8}], d: 6}]},
        {_id: 4, a: 5, b: [{c: [{f: 8}], e: 7}]},
    ],
);

// Test that $not predicates in $elemMatch can be pushed into an $or sibling of the $elemMatch.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, arr: [{a: 0, b: 2}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 1, arr: [{a: 1, b: 2}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 2, arr: [{a: 0, b: 3}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 3, arr: [{a: 1, b: 3}], c: 4, d: 5}));
assert.commandWorked(coll.insert({_id: 4, arr: [{a: 0, b: 2}], c: 6, d: 7}));
assert.commandWorked(coll.insert({_id: 5, arr: [{a: 1, b: 2}], c: 6, d: 7}));
assert.commandWorked(coll.insert({_id: 6, arr: [{a: 0, b: 3}], c: 6, d: 7}));
assert.commandWorked(coll.insert({_id: 7, arr: [{a: 1, b: 3}], c: 6, d: 7}));

const keyPattern = {
    "arr.a": 1,
    "arr.b": 1,
    c: 1,
    d: 1,
};
assert.commandWorked(coll.createIndex(keyPattern));

const elemMatchOr = {
    arr: {$elemMatch: {a: {$ne: 1}, $or: [{b: 2}, {b: 3}]}},
    $or: [
        {c: 4, d: 5},
        {c: 6, d: 7},
    ],
};

// Confirm that we get the same results using the index and a COLLSCAN.
for (let hint of [keyPattern, {$natural: 1}]) {
    assert.eq(coll.find(elemMatchOr, {_id: 1}).sort({_id: 1}).hint(hint).toArray(), [
        {_id: 0},
        {_id: 2},
        {_id: 4},
        {_id: 6},
    ]);

    assert.eq(
        coll
            .aggregate(
                [
                    {
                        $match: {
                            arr: {$elemMatch: {a: {$ne: 1}}},
                            $or: [
                                {c: 4, d: 5},
                                {c: 6, d: 7},
                            ],
                        },
                    },
                    {$project: {_id: 1}},
                    {$sort: {_id: 1}},
                ],
                {hint: hint},
            )
            .toArray(),
        [{_id: 0}, {_id: 2}, {_id: 4}, {_id: 6}],
    );
}

// SERVER-125857: a negation can be pushed into an $or under the same $elemMatch. Plan shape is
// asserted in query_planner_array_test.cpp; this covers only the returned documents.
coll.drop();
assert.commandWorked(
    coll.insert([
        // Should match: a single element satisfies both {a: {$ne: 1}} and the $or.
        {_id: 0, arr: [{a: 0, b: 1}]},
        {_id: 1, arr: [{a: 0, b: 5}]},
        {
            _id: 2,
            arr: [
                {a: 1, b: 1},
                {a: 0, b: 5},
            ],
        },
        {
            _id: 3,
            arr: [
                {a: 2, b: 0},
                {a: 1, b: 9},
            ],
        },
        // Should not match: no single element satisfies both halves of the conjunction.
        {_id: 4, arr: [{a: 1, b: 1}]},
        {_id: 5, arr: [{a: 0, b: 3}]},
        {
            _id: 6,
            arr: [
                {a: 1, b: 1},
                {a: 0, b: 3},
            ],
        },
        {
            _id: 7,
            arr: [
                {a: 1, b: 5},
                {a: 0, b: 3},
            ],
        },
    ]),
);

const negationKeyPattern = {"arr.a": 1, "arr.b": 1};
assert.commandWorked(coll.createIndex(negationKeyPattern));

const negationUnderElemMatch = {
    arr: {$elemMatch: {a: {$ne: 1}, $or: [{b: {$lt: 2}}, {b: {$gt: 3}}]}},
};

// Confirm that we get the same results using the index and a COLLSCAN.
assertSameResultsWithIndexAndCollscan(
    negationKeyPattern,
    negationUnderElemMatch,
    [0, 1, 2, 3],
    "unexpected results for $elemMatch with negation and contained $or",
);

// As above, but the negated path contains a nested array 'arr.a', so the
// negation is applied to all of the 'a.c' values of a single 'arr' element at once. The index bounds
// over-approximate, and the residual $elemMatch filter must reject the extra documents.
coll.drop();
assert.commandWorked(
    coll.insert([
        // Should match: an element whose 'a.c' values are all != 1, and whose 'b' satisfies the $or.
        {_id: 0, arr: [{a: [{c: 2}], b: 1}]},
        {_id: 1, arr: [{a: [{c: 2}], b: 5}]},
        {_id: 2, arr: [{a: [{c: 2}, {c: 3}], b: 0}]},
        // Matches on the second arr element only.
        {
            _id: 3,
            arr: [
                {a: [{c: 1}, {c: 2}], b: 0},
                {a: [{c: 5}], b: 9},
            ],
        },
        // Missing 'a.c' is not equal to 1, so the negation is satisfied.
        {_id: 4, arr: [{b: 1}]},
        // Should not match: the element contains 'a.c' equal to 1, so {$ne: 1} is false for it even
        // though the element also has other 'a.c' values which are not 1.
        {_id: 5, arr: [{a: [{c: 1}, {c: 2}], b: 1}]},
        {_id: 6, arr: [{a: [{c: 1}], b: 5}]},
        // Should not match: 'b' fails the $or.
        {_id: 7, arr: [{a: [{c: 2}], b: 3}]},
        // Should not match: no single element satisfies both halves of the conjunction.
        {
            _id: 8,
            arr: [
                {a: [{c: 1}], b: 0},
                {a: [{c: 2}], b: 3},
            ],
        },
    ]),
);

const nestedNegationKeyPattern = {"arr.a.c": 1, "arr.b": 1};
assert.commandWorked(coll.createIndex(nestedNegationKeyPattern));

const nestedNegationUnderElemMatch = {
    arr: {$elemMatch: {"a.c": {$ne: 1}, $or: [{b: {$lt: 2}}, {b: {$gt: 3}}]}},
};

// Confirm that we get the same results using the index and a COLLSCAN.
assertSameResultsWithIndexAndCollscan(
    nestedNegationKeyPattern,
    nestedNegationUnderElemMatch,
    [0, 1, 2, 3, 4],
    "unexpected results for $elemMatch with negation on a nested array and contained $or",
);
