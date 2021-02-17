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

for (let doc of kDocs) {
    coll.insert(doc);
}

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
assert.commandWorked(coll2.insert({_id: 1, "b": "hello"}));
assert.commandWorked(coll2.insert({_id: 2, "b": {"0": "hello"}}));
assert.commandWorked(coll2.insert({_id: 3, "b": ["hello", "abc", "abc"]}));

// Regexes are often something of a special case.
{
    const res = coll2.find({"b.0": {$regex: "hello"}}).toArray();
    const expected = [{_id: 2, "b": {"0": "hello"}}, {_id: 3, "b": ["hello", "abc", "abc"]}];

    assert.sameMembers(res, expected);
}
})();
