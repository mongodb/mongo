/**
 * Tests some of the find command's semantics with respect to how 64-bit integers and doubles are
 * compared with each other.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const coll = db.jstests_compare_long_double;
coll.drop();

function runWithAndWithoutIndex(keyPattern, testFunc) {
    testFunc();
    assert.commandWorked(coll.createIndex(keyPattern));
    testFunc();
}

const values = [
    {_id: 125, a: 0, b: 0, c: -(Math.pow(2, 63) + Math.pow(2, 11))},
    {_id: 101, a: 1, b: 0, c: NumberLong("-9223372036854775808")},
    {_id: 112, a: 1, b: 1, c: -(Math.pow(2, 63))},
    {_id: 132, a: 2, b: 0, c: NumberLong("-9223372036854775807")},
    {_id: 106, a: 3, b: 0, c: NumberLong("-9223372036854773761")},
    {_id: 126, a: 4, b: 0, c: NumberLong("-9223372036854773760")},
    {_id: 131, a: 4, b: 1, c: -(Math.pow(2, 63) - Math.pow(2, 11))},
    {_id: 122, a: 5, b: 0, c: NumberLong("-9223372036854773759")},
    {_id: 111, a: 6, b: 0, c: NumberLong("-9007199254740996")},
    {_id: 123, a: 6, b: 1, c: -9007199254740996},
    {_id: 117, a: 7, b: 0, c: NumberLong("-9007199254740995")},
    {_id: 103, a: 8, b: 0, c: NumberLong("-9007199254740994")},
    {_id: 114, a: 8, b: 1, c: -9007199254740994},
    {_id: 109, a: 9, b: 0, c: NumberLong("-9007199254740993")},
    {_id: 116, a: 10, b: 0, c: NumberLong("-9007199254740992")},
    {_id: 124, a: 10, b: 1, c: -9007199254740992},
    {_id: 129, a: 11, b: 0, c: NumberLong("-9007199254740991")},
    {_id: 130, a: 11, b: 1, c: -9007199254740991},
    {_id: 105, a: 12, b: 0, c: NumberLong("9007199254740991")},
    {_id: 115, a: 12, b: 1, c: 9007199254740991},
    {_id: 113, a: 13, b: 0, c: NumberLong("9007199254740992")},
    {_id: 120, a: 13, b: 1, c: 9007199254740992},
    {_id: 108, a: 14, b: 0, c: NumberLong("9007199254740993")},
    {_id: 107, a: 15, b: 0, c: NumberLong("9007199254740994")},
    {_id: 110, a: 15, b: 1, c: 9007199254740994},
    {_id: 121, a: 16, b: 0, c: NumberLong("9007199254740995")},
    {_id: 118, a: 17, b: 0, c: NumberLong("9007199254740996")},
    {_id: 100, a: 17, b: 1, c: 9007199254740996},
    {_id: 104, a: 18, b: 0, c: NumberLong("9223372036854773759")},
    {_id: 127, a: 19, b: 0, c: NumberLong("9223372036854773760")},
    {_id: 133, a: 19, b: 1, c: (Math.pow(2, 63) - Math.pow(2, 11))},
    {_id: 128, a: 20, b: 0, c: NumberLong("9223372036854773761")},
    {_id: 102, a: 21, b: 0, c: NumberLong("9223372036854775807")},
    {_id: 119, a: 22, b: 0, c: Math.pow(2, 63)},
];

Random.setRandomSeed(0);

assert.commandWorked(coll.insert(Array.shuffle(values.concat())));

runWithAndWithoutIndex({a: 1}, () => {
    const testcase = function(query, lambda) {
        const expected = values
                             .map(x => {
                                 return {a: x.a};
                             })
                             .filter(lambda);
        const result = coll.find(query, {a: 1, _id: 0}).toArray();
        assert(arrayEq(result, expected),
               tojson(query) + " failed:\n" + tojson(result) + " != " + tojson(expected));
    };

    for (const {_id: id, a: a, c: c} of values) {
        testcase({c: {$lt: c}}, x => (x.a < a));
        testcase({c: {$lte: c}}, x => (x.a <= a));
        testcase({c: {$eq: c}}, x => (x.a == a));
        testcase({c: {$gte: c}}, x => (x.a >= a));
        testcase({c: {$gt: c}}, x => (x.a > a));

        testcase({$expr: {$lt: ["$c", c]}}, x => (x.a < a));
        testcase({$expr: {$lte: ["$c", c]}}, x => (x.a <= a));
        testcase({$expr: {$eq: ["$c", c]}}, x => (x.a == a));
        testcase({$expr: {$gte: ["$c", c]}}, x => (x.a >= a));
        testcase({$expr: {$gt: ["$c", c]}}, x => (x.a > a));

        const result = coll.find({}).sort({c: 1, b: 1}).toArray().map(x => x._id);
        const expected = coll.find({}).sort({a: 1, b: 1}).toArray().map(x => x._id);
        assert(orderedArrayEq(result, expected),
               "Comparison of sort results failed:\n" + tojson(result) + " != " + tojson(expected));
    }
});
})();
