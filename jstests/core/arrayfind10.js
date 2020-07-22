/**
 * Tests some of the find command's semantics with respect to how arrays are handled.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // arrayEq

const t = db.jstests_arrayfind10;
t.drop();

function runWithAndWithoutIndex(keyPattern, testFunc) {
    testFunc();
    assert.commandWorked(t.createIndex(keyPattern));
    testFunc();
}

assert.commandWorked(t.insert([{a: "foo"}, {a: ["foo"]}, {a: [["foo"]]}, {a: [[["foo"]]]}]));

runWithAndWithoutIndex({a: 1}, () => {
    assert(arrayEq(t.find({a: "foo"}, {_id: 0}).toArray(), [{a: "foo"}, {a: ["foo"]}]));
});

assert(t.drop());

assert.commandWorked(t.insert([
    {a: [123, "foo"]},
    {a: ["foo", 123]},
    {a: ["bar", "foo"]},
    {a: ["bar", "baz", "foo"]},
    {a: ["bar", "baz", 123]}
]));

runWithAndWithoutIndex({a: 1}, () => {
    assert(arrayEq(
        t.find({a: "foo"}, {_id: 0}).toArray(),
        [{a: [123, "foo"]}, {a: ["foo", 123]}, {a: ["bar", "foo"]}, {a: ["bar", "baz", "foo"]}]));
});

assert(t.drop());

assert.commandWorked(t.insert([
    {a: [{}, {b: "foo"}]},
    {a: [{b: "foo"}, {}]},
    {a: [{b: 123}, {b: "foo"}]},
    {a: [{b: "foo"}, {b: 123}]},
    {a: [{b: "bar"}, {b: "foo"}]},
    {a: [{b: "bar"}, {b: "baz"}, {b: "foo"}]},
    {a: [{b: "bar"}, {b: "baz"}, {b: 123}]}
]));

runWithAndWithoutIndex({"a.b": 1}, () => {
    assert(arrayEq(t.find({"a.b": "foo"}, {_id: 0}).toArray(), [
        {a: [{}, {b: "foo"}]},
        {a: [{b: "foo"}, {}]},
        {a: [{b: 123}, {b: "foo"}]},
        {a: [{b: "foo"}, {b: 123}]},
        {a: [{b: "bar"}, {b: "foo"}]},
        {a: [{b: "bar"}, {b: "baz"}, {b: "foo"}]}
    ]));
});
})();
