/**
 * Test indexes with long paths, where some components are multikey and some are not.
 * Make sure queries can use these indexes, with good bounds.
 *
 * @tags: [
 *   # Checks explain.
 *   requires_cqf,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For leftmostLeafStage

db.setLogLevel(4, "query");

const coll = db.cqf_non_multikey_paths;
coll.drop();
assert.commandWorked(coll.createIndex({'one.one.one.one': 1}));
assert.commandWorked(coll.createIndex({'one.one.one.many': 1}));
assert.commandWorked(coll.createIndex({'many.one.one.one': 1}));
assert.commandWorked(coll.createIndex({'many.one.one.many': 1}));
assert.commandWorked(coll.createIndex({'many.many.many.many': 1}));
let i = 0;
assert.commandWorked(coll.insert([
    {_id: ++i, one: {one: {one: {one: 2}}}},
    {_id: ++i, one: {one: {one: {many: [1, 2, 3]}}}},
    {
        _id: ++i,
        many: [
            {one: {one: {one: 1}}},
            {one: {one: {one: 2}}},
            {one: {one: {one: 3}}},
        ]
    },
    {
        _id: ++i,
        many: [
            {one: {one: {many: [1, 2, 3]}}},
            {one: {one: {many: [4, 5]}}},
        ],
    },
    {_id: ++i, many: [{many: [{many: [{many: [1, 2, 3]}]}]}]},
]));
// Generate enough documents for index to be preferable.
assert.commandWorked(coll.insert(Array.from({length: 100}, (_, i) => ({_id: i + 1000}))));

function run(pipeline) {
    jsTestLog(`Query: ${tojsononeline(pipeline)}`);
    const explain = coll.explain().aggregate(pipeline);
    print(`Leaf stage: `);
    const {nodeType, indexDefName, interval} = leftmostLeafStage(explain);
    if (nodeType === undefined) {
        printjson({message: "nodeType is undefined, which should not be possible.", explain});
        return;
    }
    printjson({nodeType, indexDefName, interval: prettyInterval(interval)});
}

run([{$match: {'one.one.one.one': 2}}]);
run([{$match: {'one.one.one.many': 2}}]);
run([{$match: {'many.one.one.one': 2}}]);
run([{$match: {'many.one.one.many': 2}}]);
run([{$match: {'many.many.many.many': 2}}]);
})();
