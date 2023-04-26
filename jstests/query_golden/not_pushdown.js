/**
 * Test query predicates with combinations of $not and array traversal.
 * When possible, we should remove Traverse nodes.
 *
 * @tags: [
 *   # Checks explain.
 *   requires_cqf,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For leftmostLeafStage

const coll = db.cqf_not_pushdown;
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

function run(note, pipeline) {
    jsTestLog(`Query: ${tojsononeline(pipeline)}\nnote: ${note}`);

    print(`Operators used: `);
    const explain = coll.explain().aggregate(pipeline);
    const ops =
        findSubtrees(explain, node => node.op === 'Not' || node.op === 'Eq' || node.op === 'Neq')
            .map(node => node.op);
    printjson(ops);
}

// Simple case: non-multikey.
run('Should be optimized to Neq', [{$match: {'one.one.one.one': {$ne: 7}}}]);

// Multikey case: we have Traverse in between Not and Eq, which is not the same as Neq.
run('Should stay as Not Traverse Eq', [{$match: {'one.one.one.many': {$ne: 7}}}]);
run('Should stay as Not Traverse Eq', [{$match: {'many.one.one.one': {$ne: 7}}}]);
run('Should stay as Not Traverse Eq', [{$match: {'many.one.one.many': {$ne: 7}}}]);
run('Should stay as Not Traverse Eq', [{$match: {'many.many.many.many': {$ne: 7}}}]);

// We have an $elemMatch (multikey), but no Traverse underneath the Not.
run('Should be optimized to Neq', [{$match: {'many': {$elemMatch: {'one.one.one': {$ne: 7}}}}}]);
run('Should be optimized to Neq', [{$match: {'many.one': {$elemMatch: {'one.one': {$ne: 7}}}}}]);
})();
