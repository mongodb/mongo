/**
 * Tests behavior of the match language when using numeric path components.
 */
(function() {
const coll = db.match_numeric_components;
coll.drop();

const kDocs = [
    {_id: 0, "a": 42},
    {_id: 1, "a": [42]},
    {_id: 2, "a": {"0": 42}},
    {_id: 3, "a": [[42]]},
    {_id: 4, "a": [{"0": 42}]},
    {_id: 5, "a": {"0": [42]}},
    {_id: 6, "a": {"0": {"0": 42}}},
    {_id: 7, "a": [[[42]]]},
    {_id: 8, "a": [[{"0": 42}]]},
    {_id: 9, "a": [{"0": [42]}]},
    {_id: 10, "a": [{"0": {"0": 42}}]},
    {_id: 11, "a": {"0": [[42]]}},
    {_id: 12, "a": {"0": [{"0": 42}]}},
    {_id: 13, "a": {"0": {"0": [42]}}},
    {_id: 14, "a": {"0": {"0": {"0": 42}}}},
    {_id: 15, "a": [[[[42]]]]},
    {_id: 16, "a": [[[{"0": 42}]]]},
    {_id: 17, "a": [[{"0": [42]}]]},
    {_id: 18, "a": [[{"0": {"0": 42}}]]},
    {_id: 19, "a": [{"0": [[42]]}]},
    {_id: 20, "a": [{"0": [{"0": 42}]}]},
    {_id: 21, "a": [{"0": {"0": [42]}}]},
    {_id: 22, "a": [{"0": {"0": {"0": 42}}}]},
    {_id: 23, "a": {"0": [[[42]]]}},
    {_id: 24, "a": {"0": [[{"0": 42}]]}},
    {_id: 25, "a": {"0": [{"0": [42]}]}},
    {_id: 26, "a": {"0": [{"0": {"0": 42}}]}},
    {_id: 27, "a": {"0": {"0": [[42]]}}},
    {_id: 28, "a": {"0": {"0": [{"0": 42}]}}},
    {_id: 29, "a": {"0": {"0": {"0": [42]}}}},
    {_id: 30, "a": {"0": {"0": {"0": {"0": 42}}}}},
];

assert.commandWorked(coll.insert(kDocs));

{
    const res = coll.find({"a.0": 42}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]}
    ];

    assert.sameMembers(res, expected);
}

// Using $ne.
{
    const res = coll.find({"a.0": {$ne: 42}}).toArray();
    const expected = [
        {_id: 0, "a": 42},
        {_id: 3, "a": [[42]]},
        {_id: 6, "a": {"0": {"0": 42}}},
        {_id: 7, "a": [[[42]]]},
        {_id: 8, "a": [[{"0": 42}]]},
        {_id: 10, "a": [{"0": {"0": 42}}]},
        {_id: 11, "a": {"0": [[42]]}},
        {_id: 12, "a": {"0": [{"0": 42}]}},
        {_id: 13, "a": {"0": {"0": [42]}}},
        {_id: 14, "a": {"0": {"0": {"0": 42}}}},
        {_id: 15, "a": [[[[42]]]]},
        {_id: 16, "a": [[[{"0": 42}]]]},
        {_id: 17, "a": [[{"0": [42]}]]},
        {_id: 18, "a": [[{"0": {"0": 42}}]]},
        {_id: 19, "a": [{"0": [[42]]}]},
        {_id: 20, "a": [{"0": [{"0": 42}]}]},
        {_id: 21, "a": [{"0": {"0": [42]}}]},
        {_id: 22, "a": [{"0": {"0": {"0": 42}}}]},
        {_id: 23, "a": {"0": [[[42]]]}},
        {_id: 24, "a": {"0": [[{"0": 42}]]}},
        {_id: 25, "a": {"0": [{"0": [42]}]}},
        {_id: 26, "a": {"0": [{"0": {"0": 42}}]}},
        {_id: 27, "a": {"0": {"0": [[42]]}}},
        {_id: 28, "a": {"0": {"0": [{"0": 42}]}}},
        {_id: 29, "a": {"0": {"0": {"0": [42]}}}},
        {_id: 30, "a": {"0": {"0": {"0": {"0": 42}}}}},
    ];
    assert.sameMembers(res, expected);
}

{
    const res = coll.find({"a.0.0": 42}).toArray();
    const expected = [
        {_id: 3, "a": [[42]]},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 6, "a": {"0": {"0": 42}}},
        {_id: 8, "a": [[{"0": 42}]]},
        {_id: 9, "a": [{"0": [42]}]},
        {_id: 10, "a": [{"0": {"0": 42}}]},
        {_id: 12, "a": {"0": [{"0": 42}]}},
        {_id: 13, "a": {"0": {"0": [42]}}},
        {_id: 17, "a": [[{"0": [42]}]]},
        {_id: 20, "a": [{"0": [{"0": 42}]}]},
        {_id: 21, "a": [{"0": {"0": [42]}}]},
        {_id: 25, "a": {"0": [{"0": [42]}]}}
    ];

    assert.sameMembers(res, expected);
}

// Using a comparison.
{
    const res = coll.find({"a.0": {$gt: 41}}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]}
    ];

    assert.sameMembers(res, expected);
}

// Using $in.
{
    const res = coll.find({"a.0": {$in: [41, 42, 43]}}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]}
    ];

    assert.sameMembers(res, expected);
}

// Using $nin.
{
    const res = coll.find({"a.0": {$nin: [41, 42, 43]}}).toArray();
    const expected = [
        {_id: 0, "a": 42},
        {_id: 3, "a": [[42]]},
        {_id: 6, "a": {"0": {"0": 42}}},
        {_id: 7, "a": [[[42]]]},
        {_id: 8, "a": [[{"0": 42}]]},
        {_id: 10, "a": [{"0": {"0": 42}}]},
        {_id: 11, "a": {"0": [[42]]}},
        {_id: 12, "a": {"0": [{"0": 42}]}},
        {_id: 13, "a": {"0": {"0": [42]}}},
        {_id: 14, "a": {"0": {"0": {"0": 42}}}},
        {_id: 15, "a": [[[[42]]]]},
        {_id: 16, "a": [[[{"0": 42}]]]},
        {_id: 17, "a": [[{"0": [42]}]]},
        {_id: 18, "a": [[{"0": {"0": 42}}]]},
        {_id: 19, "a": [{"0": [[42]]}]},
        {_id: 20, "a": [{"0": [{"0": 42}]}]},
        {_id: 21, "a": [{"0": {"0": [42]}}]},
        {_id: 22, "a": [{"0": {"0": {"0": 42}}}]},
        {_id: 23, "a": {"0": [[[42]]]}},
        {_id: 24, "a": {"0": [[{"0": 42}]]}},
        {_id: 25, "a": {"0": [{"0": [42]}]}},
        {_id: 26, "a": {"0": [{"0": {"0": 42}}]}},
        {_id: 27, "a": {"0": {"0": [[42]]}}},
        {_id: 28, "a": {"0": {"0": [{"0": 42}]}}},
        {_id: 29, "a": {"0": {"0": {"0": [42]}}}},
        {_id: 30, "a": {"0": {"0": {"0": {"0": 42}}}}},
    ];

    assert.sameMembers(res, expected);
}

// Using $exists with true and false.
{
    let res = coll.find({"a.0": {$exists: false}}).toArray();
    let expected = [{_id: 0, "a": 42}];

    assert.sameMembers(res, expected);

    res = coll.find({"a.0": {$exists: true}}).toArray();
    expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 3, "a": [[42]]},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 6, "a": {"0": {"0": 42}}},
        {_id: 7, "a": [[[42]]]},
        {_id: 8, "a": [[{"0": 42}]]},
        {_id: 9, "a": [{"0": [42]}]},
        {_id: 10, "a": [{"0": {"0": 42}}]},
        {_id: 11, "a": {"0": [[42]]}},
        {_id: 12, "a": {"0": [{"0": 42}]}},
        {_id: 13, "a": {"0": {"0": [42]}}},
        {_id: 14, "a": {"0": {"0": {"0": 42}}}},
        {_id: 15, "a": [[[[42]]]]},
        {_id: 16, "a": [[[{"0": 42}]]]},
        {_id: 17, "a": [[{"0": [42]}]]},
        {_id: 18, "a": [[{"0": {"0": 42}}]]},
        {_id: 19, "a": [{"0": [[42]]}]},
        {_id: 20, "a": [{"0": [{"0": 42}]}]},
        {_id: 21, "a": [{"0": {"0": [42]}}]},
        {_id: 22, "a": [{"0": {"0": {"0": 42}}}]},
        {_id: 23, "a": {"0": [[[42]]]}},
        {_id: 24, "a": {"0": [[{"0": 42}]]}},
        {_id: 25, "a": {"0": [{"0": [42]}]}},
        {_id: 26, "a": {"0": [{"0": {"0": 42}}]}},
        {_id: 27, "a": {"0": {"0": [[42]]}}},
        {_id: 28, "a": {"0": {"0": [{"0": 42}]}}},
        {_id: 29, "a": {"0": {"0": {"0": [42]}}}},
        {_id: 30, "a": {"0": {"0": {"0": {"0": 42}}}}},
    ];

    assert.sameMembers(res, expected);
}

// Using $elemMatch.
{
    const res = coll.find({a: {$elemMatch: {"0.0": {$eq: 42}}}}).toArray();
    const expected = [
        {_id: 7, "a": [[[42]]]},
        {_id: 8, "a": [[{"0": 42}]]},
        {_id: 9, "a": [{"0": [42]}]},
        {_id: 10, "a": [{"0": {"0": 42}}]},
        {_id: 16, "a": [[[{"0": 42}]]]},
        {_id: 17, "a": [[{"0": [42]}]]},
        {_id: 20, "a": [{"0": [{"0": 42}]}]},
        {_id: 21, "a": [{"0": {"0": [42]}}]},
    ];

    assert.sameMembers(res, expected);
}

// Using top-level $and.
{
    const res = coll.find({_id: {$lt: 15}, "a.0": {$gt: 41}}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]},
    ];

    assert.sameMembers(res, expected);
}

// $all with equality
{
    const res = coll.find({"a.0": {$all: [42]}}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]}
    ];

    assert.sameMembers(res, expected);
}

// $all with $elemMatch
{
    const res = coll.find({"a.0": {$all: [{$elemMatch: {0: 42}}]}}).toArray();
    const expected = [
        {_id: 7, "a": [[[42]]]},
        {_id: 8, "a": [[{"0": 42}]]},
        {_id: 11, "a": {"0": [[42]]}},
        {_id: 12, "a": {"0": [{"0": 42}]}},
        {_id: 15, "a": [[[[42]]]]},
        {_id: 17, "a": [[{"0": [42]}]]},
        {_id: 19, "a": [{"0": [[42]]}]},
        {_id: 20, "a": [{"0": [{"0": 42}]}]},
        {_id: 23, "a": {"0": [[[42]]]}},
        {_id: 25, "a": {"0": [{"0": [42]}]}},
    ];

    assert.sameMembers(res, expected);
}

// Using an expression.
{
    const res = coll.find({"a.0": {$type: "number"}}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]}
    ];

    assert.sameMembers(res, expected);
}

{
    const res = coll.find({"a.0": {$mod: [42, 0]}}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]}
    ];

    assert.sameMembers(res, expected);
}

// In a $or.
{
    const res = coll.find({$or: [{"a.0": 42}, {"notARealField": 123}]}).toArray();
    const expected = [
        {_id: 1, "a": [42]},
        {_id: 2, "a": {"0": 42}},
        {_id: 4, "a": [{"0": 42}]},
        {_id: 5, "a": {"0": [42]}},
        {_id: 9, "a": [{"0": [42]}]}
    ];

    assert.sameMembers(res, expected);
}

const coll2 = db.match_numeric_components2;
coll2.drop();

const kRegexDocs =
    [{_id: 1, "b": "hello"}, {_id: 2, "b": {"0": "hello"}}, {_id: 3, "b": ["hello", "abc", "abc"]}];

assert.commandWorked(coll2.insert(kRegexDocs));

// Regexes are often something of a special case.
{
    const res = coll2.find({"b.0": {$regex: "hello"}}).toArray();
    const expected = [{_id: 2, "b": {"0": "hello"}}, {_id: 3, "b": ["hello", "abc", "abc"]}];

    assert.sameMembers(res, expected);
}

// $all with regexes.
{
    const res = coll2.find({"b.0": {$all: [/^hello/]}}).toArray();
    const expected = [{_id: 2, "b": {"0": "hello"}}, {_id: 3, "b": ["hello", "abc", "abc"]}];
    assert.sameMembers(res, expected);
}

// $not with regex.
{
    const res = coll2.find({"b.0": {$not: /^h/}}).toArray();
    const expected = [{_id: 1, "b": "hello"}];
    assert.sameMembers(res, expected);
}

// The tests below indirectly make sure that an index scan is not chosen when $elemMatch is
// against a indexed positional path component because it is not possible to generate index
// bounds from the $elemMatch conditions. If an index scan is chosen, then the corresponding
// queries would produce a wrong result.
// More precisely for an index like {"a.0": 1} and a document {a: [[1, 2, 3]]}, the nested array is
// not unwound during index key generation. That is, there is a single index key {"": [1, 2, 3]}
// rather than three separate index keys, {"": 1}, {"": 2}, {"": 3}. This precludes the ability to
// generate index bounds for $elemMatch predicates on "a.0" because "a.0" refers to the whole array
// [1, 2, 3], and not its individual members.
{
    // Test with $in.
    assert(coll.drop());
    const expectedDoc = {_id: 42, "a": [["b"], ["c"]]};
    assert.commandWorked(coll.insert(expectedDoc));
    const query = {"a.0": {$elemMatch: {$in: ["b"]}}};

    const res1 = coll.find(query).toArray();
    assert.commandWorked(coll.createIndex({"a.0": 1}));
    const res2 = coll.find(query).toArray();
    assert.sameMembers([expectedDoc], res1);
    assert.sameMembers([expectedDoc], res2);
}

// Tests with equality. Add some data for the next few tests.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, f0: 'zz', f1: 5}));
assert.commandWorked(coll.insert({_id: 1, f0: 'zz', f1: [3, 5]}));
assert.commandWorked(coll.insert({_id: 4, f0: 'zz', f1: [3, 5, [7, 9]]}));
assert.commandWorked(coll.insert({_id: 2, f0: 'zz', f1: [[3, 5], [5, 7]]}));
assert.commandWorked(coll.insert({_id: 3, f0: 'zz', f1: [[[0], [3, 5]], [[0], [5, 7]]]}));

{
    const res1 = coll.find({'f1.0': {$elemMatch: {$eq: 5}}}).toArray();
    const res2 = coll.find({'f1.0': {$elemMatch: {$eq: [3, 5]}}}).toArray();
    assert.commandWorked(coll.createIndex({'f1.0': 1}));
    const res3 = coll.find({'f1.0': {$elemMatch: {$eq: 5}}}).toArray();
    const res4 = coll.find({'f1.0': {$elemMatch: {$eq: [3, 5]}}}).toArray();
    const expected1 = [{_id: 2, f0: 'zz', f1: [[3, 5], [5, 7]]}];
    const expected2 = [{_id: 3, f0: 'zz', f1: [[[0], [3, 5]], [[0], [5, 7]]]}];
    assert.sameMembers(expected1, res1);
    assert.sameMembers(expected1, res3);
    assert.sameMembers(expected2, res2);
    assert.sameMembers(expected2, res4);
    assert.commandWorked(coll.dropIndex({'f1.0': 1}));
}

{
    // Compound index.
    const res1 = coll.find({'f0': 'zz', 'f1.0': {$elemMatch: {$eq: 5}}}).toArray();
    assert.commandWorked(coll.createIndex({'f0': 1, 'f1.0': 1}));
    const res2 = coll.find({'f0': 'zz', 'f1.0': {$elemMatch: {$eq: 5}}}).toArray();
    const expected = [{_id: 2, f0: 'zz', f1: [[3, 5], [5, 7]]}];
    assert.sameMembers(expected, res1);
    assert.sameMembers(expected, res2);
    assert.commandWorked(coll.dropIndex({'f0': 1, 'f1.0': 1}));
}

{
    // Two-levels of array nesting.
    const res1 = coll.find({'f1.0.1': {$elemMatch: {$eq: 3}}}).toArray();
    assert.commandWorked(coll.createIndex({'f1.0.1': 1}));
    const res2 = coll.find({'f1.0.1': {$elemMatch: {$eq: 3}}}).toArray();
    const expected = [{_id: 3, f0: 'zz', f1: [[[0], [3, 5]], [[0], [5, 7]]]}];
    assert.sameMembers(expected, res1);
    assert.sameMembers(expected, res2);
}

{
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({'f1.0': 1}));
    assert.commandWorked(coll.insert({_id: 1, f1: [[42, 5], [77, 99]]}));

    const res1 = coll.find({'f1.0': {$elemMatch: {$eq: 5}}}).toArray();
    assert.sameMembers([{_id: 1, f1: [[42, 5], [77, 99]]}], res1);

    // Object with numeric field component, and no nested arrays.
    assert.commandWorked(coll.insert({_id: 2, f1: {0: [42, 5], 1: [77, 99]}}));
    const res2 = coll.find({'f1.0': {$elemMatch: {$eq: 5}}}).toArray();
    assert.sameMembers(
        [{_id: 1, f1: [[42, 5], [77, 99]]}, {_id: 2, f1: {'0': [42, 5], '1': [77, 99]}}], res2);
}

{
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({'0': 1}));

    assert.commandWorked(coll.insert({_id: 1, '0': [42, 5]}));
    const res1 = coll.find({'0': {$elemMatch: {$eq: 5}}}).toArray();
    assert.sameMembers([{'0': [42, 5], _id: 1}], res1);

    assert.commandWorked(coll.createIndex({'0.1': 1}));
    assert.commandWorked(coll.insert({_id: 2, '0': {0: [42], 1: [5]}}));
    const res2 = coll.find({'0.1': {$elemMatch: {$eq: 5}}}).toArray();
    assert.sameMembers([{'0': {'0': [42], '1': [5]}, _id: 2}], res2);
}
})();
