/**
 * Tests some of the find command's semantics with respect to object comparisons.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const t = db.jstests_objectfind;
t.drop();

function runWithAndWithoutIndex(keyPattern, testFunc) {
    testFunc();
    assert.commandWorked(t.createIndex(keyPattern));
    testFunc();
}

assert.commandWorked(t.insert([
    {a: {b: "d", c: "e"}},
    {a: {c: "b", e: "d"}},
    {a: {c: "b", e: "d", h: "g"}},
    {a: {c: "b", e: {a: "a"}}},
    {a: {c: "d", e: "b"}},
    {a: {d: "b", e: "c"}},
    {a: {d: "c"}},
    {a: {d: "c", b: "e"}},
    {a: {d: "c", e: "b"}},
    {a: {d: "c", e: "b", g: "h"}},
    {a: {d: "c", e: "f"}},
    {a: {d: "e", b: "c"}},
    {a: {d: "e", c: "b"}},
    {a: {e: "b", d: "c"}},
    {a: {e: "d", c: "b"}}
]));

runWithAndWithoutIndex({a: 1}, () => {
    assert(
        arrayEq(t.find({a: {$eq: {d: "c", e: "b"}}}, {_id: 0}).toArray(), [{a: {d: "c", e: "b"}}]));

    assert(arrayEq(t.find({a: {$lt: {d: "c", e: "b"}}}, {_id: 0}).toArray(), [
        {a: {b: "d", c: "e"}},
        {a: {c: "b", e: "d"}},
        {a: {c: "b", e: "d", h: "g"}},
        {a: {c: "b", e: {a: "a"}}},
        {a: {c: "d", e: "b"}},
        {a: {d: "b", e: "c"}},
        {a: {d: "c"}},
        {a: {d: "c", b: "e"}},
    ]));

    assert(arrayEq(t.find({a: {$gt: {d: "c", e: "b"}}}, {_id: 0}).toArray(), [
        {a: {d: "c", e: "b", g: "h"}},
        {a: {d: "c", e: "f"}},
        {a: {d: "e", b: "c"}},
        {a: {d: "e", c: "b"}},
        {a: {e: "b", d: "c"}},
        {a: {e: "d", c: "b"}}
    ]));

    assert(
        arrayEq(t.find({a: {$eq: {c: "b", e: "d"}}}, {_id: 0}).toArray(), [{a: {c: "b", e: "d"}}]));

    assert(
        arrayEq(t.find({a: {$lt: {c: "b", e: "d"}}}, {_id: 0}).toArray(), [{a: {b: "d", c: "e"}}]));

    assert(arrayEq(t.find({a: {$gt: {c: "b", e: "d"}}}, {_id: 0}).toArray(), [
        {a: {c: "b", e: "d", h: "g"}},
        {a: {c: "b", e: {a: "a"}}},
        {a: {c: "d", e: "b"}},
        {a: {d: "b", e: "c"}},
        {a: {d: "c"}},
        {a: {d: "c", b: "e"}},
        {a: {d: "c", e: "b"}},
        {a: {d: "c", e: "b", g: "h"}},
        {a: {d: "c", e: "f"}},
        {a: {d: "e", b: "c"}},
        {a: {d: "e", c: "b"}},
        {a: {e: "b", d: "c"}},
        {a: {e: "d", c: "b"}}
    ]));

    assert.eq(0, t.find({a: {$eq: {}}}).itcount());
    assert.eq(0, t.find({a: {$lt: {}}}).itcount());
    assert.eq(15, t.find({a: {$gt: {}}}).itcount());
});
})();
