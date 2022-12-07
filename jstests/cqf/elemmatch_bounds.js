/**
 * Test that $elemMatch can generate good index bounds. Especially, test different
 * ways that $elemMatch can lead to a conjunction inside a traverse.
 *
 * @tags: [
 *   # Includes plan skeleton from explain in the output.
 *   requires_cqf,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For getPlanSkeleton.

const coll = db.cqf_elemmatch_bounds;
coll.drop();
let id = 0;
let docs = [];
const numDuplicates = 100;
for (let i = 0; i < numDuplicates; ++i) {
    docs = docs.concat([
        // Each 'a' is an object, not an array.
        // Each 'c' is the same as 'a.b', which is a mixture of scalars/arrays.
        {_id: id++, a: {b: [1, 2, 3]}, c: [1, 2, 3]},
        {_id: id++, a: {b: 1}, c: 1},
        {_id: id++, a: {b: 2}, c: 2},
        {_id: id++, a: {b: 3}, c: 3},
        {_id: id++, a: {b: [1]}, c: [1]},
        {_id: id++, a: {b: [2]}, c: [2]},
        {_id: id++, a: {b: [3]}, c: [3]},
        // [1, 3] is interesting because it satisfies {$gt: 1} and {$lt: 3},
        // but no element satisfies both.
        {_id: id++, a: {b: [1, 3]}, c: [1, 3]},

        // Examples with nested arrays: an array element is immediately another array.
        {_id: id++, c: [[1, 2, 3]]},
        {_id: id++, c: [[1, 3]]},
        {_id: id++, c: [[1], [2], [3]]},
        {_id: id++, c: [[1], [3]]},
        {_id: id++, c: [[1]]},
        {_id: id++, c: [[2]]},
        {_id: id++, c: [[3]]},
    ]);
}
for (let i = 0; i < numDuplicates; ++i) {
    // Generate more docs where 'Get c Traverse Traverse Eq 2' but not 'Get c Traverse PathArr'.
    for (let j = 0; j < 10; ++j)
        docs = docs.concat([
            {_id: id++, c: 2},
        ]);
}
for (let i = 0; i < numDuplicates; ++i) {
    // Generate non-matching docs to discourage collection scan.
    assert.commandWorked(coll.insert(Array.from({length: 100}, () => ({_id: id++}))));
}
assert.commandWorked(coll.insert(docs));

assert.commandWorked(coll.createIndex({'a.b': 1}));
assert.commandWorked(coll.createIndex({'c': 1}));

// Run the pipeline and assert it uses the expected plan.
// Return the results with the _id field excluded.
function run({pipeline, plan: expectedPlan}) {
    const explain = coll.explain().aggregate(pipeline);
    const plan = getPlanSkeleton(explain.queryPlanner.winningPlan.optimizerPlan, {
        extraKeepKeys: ['indexDefName', 'interval'],
    });
    assert.eq(plan, expectedPlan, plan);

    let result = coll.aggregate(pipeline).toArray();
    for (let doc of result) {
        delete doc._id;
    }
    return result;
}
// Assert that 'doc' occurs exactly 'num' times in 'result'.
function assertCount(result, num, doc) {
    let matching = result.filter(d => friendlyEqual(d, doc));

    let msg = `Expected ${num} occurrences of ${tojsononeline(doc)} but got ${matching.length}.`;
    if (matching.length > 0) {
        msg += ' For example:\n  ' + matching.slice(0, 5).map(d => tojsononeline(d)).join("\n  ");
    }
    assert.eq(matching.length, num, msg);
}

let result;

// Test multikey non-$elemMatch $eq predicate includes non-arrays.
result = run({
    pipeline: [{$match: {'a.b': {$eq: 2}}}],
    plan: {
        "nodeType": "Root",
        "child": {
            "nodeType": "NestedLoopJoin",
            "leftChild": {"nodeType": "IndexScan", "indexDefName": "a.b_1", "interval": "[ 2, 2 ]"},
            "rightChild": {"nodeType": "LimitSkip", "child": {"nodeType": "Seek"}}
        }
    },
});
assertCount(result, numDuplicates, {a: {b: [1, 2, 3]}, c: [1, 2, 3]});
assertCount(result, numDuplicates, {a: {b: [2]}, c: [2]});
assertCount(result, numDuplicates, {a: {b: 2}, c: 2});
assert.eq(result.length, numDuplicates * 3);

// Test $elemMatch only matches arrays (unlike the previous query).
result = run({
    pipeline: [{$match: {'a.b': {$elemMatch: {$eq: 2}}}}],
    plan: {
        "nodeType": "Root",
        "child": {
            "nodeType": "NestedLoopJoin",
            "leftChild": {
                "nodeType": "IndexScan",
                "indexDefName": "a.b_1",
                "interval": "[ 2, 2 ]",
            },
            "rightChild": {
                "nodeType": "Filter",
                "child": {
                    "nodeType": "LimitSkip",
                    "child": {"nodeType": "Seek"},
                },
            }
        }
    },
});
assertCount(result, numDuplicates, {a: {b: [1, 2, 3]}, c: [1, 2, 3]});
assertCount(result, numDuplicates, {a: {b: [2]}, c: [2]});
assertCount(result, 0, {a: {b: 2}, c: 2});  // Expect zero non-arrays.
assert.eq(result.length, numDuplicates * 2);

// Test conjunction inside a top-level elemMatch.
result = run({
    pipeline: [{$match: {c: {$elemMatch: {$gt: 1, $lt: 3}}}}],
    plan: {
        "nodeType": "Root",
        "child": {
            "nodeType": "NestedLoopJoin",
            "leftChild": {
                "nodeType": "Unique",
                "child": {"nodeType": "IndexScan", "indexDefName": "c_1", "interval": "( 1, 3 )"}
            },
            "rightChild": {
                "nodeType": "Filter",
                "child": {"nodeType": "LimitSkip", "child": {"nodeType": "Seek"}}
            }
        }
    },
});
assertCount(result, numDuplicates, {a: {b: [1, 2, 3]}, c: [1, 2, 3]});
assertCount(result, numDuplicates, {a: {b: [2]}, c: [2]});
assert.eq(result.length, numDuplicates * 2);

// Test conjunction inside a dotted elemMatch.
result = run({
    pipeline: [{$match: {'a.b': {$elemMatch: {$gt: 1, $lt: 3}}}}],
    plan: {
        "nodeType": "Root",
        "child": {
            "nodeType": "NestedLoopJoin",
            "leftChild": {
                "nodeType": "Unique",
                "child": {"nodeType": "IndexScan", "indexDefName": "a.b_1", "interval": "( 1, 3 )"}
            },
            "rightChild": {
                "nodeType": "Filter",
                "child": {"nodeType": "LimitSkip", "child": {"nodeType": "Seek"}}
            }
        }
    },
});
assertCount(result, numDuplicates, {a: {b: [1, 2, 3]}, c: [1, 2, 3]});
assertCount(result, numDuplicates, {a: {b: [2]}, c: [2]});
assert.eq(result.length, numDuplicates * 2);

// Nested $elemMatch matches nested arrays, but the bounds only handle PathArr.
// Multikey indexes don't recursively unwind arrays, so the scalars inside the nested array don't
// get separate index entries, so we can't generate a tight interval like [2, 2].
result = run({
    pipeline: [{$match: {c: {$elemMatch: {$elemMatch: {$eq: 2}}}}}],
    plan: {
        "nodeType": "Root",
        "child": {
            "nodeType": "Filter",
            "child": {
                "nodeType": "NestedLoopJoin",
                "leftChild": {
                    "nodeType": "Unique",
                    "child": {
                        "nodeType": "IndexScan",
                        "indexDefName": "c_1",
                        "interval": "[ [ ], BinData(0,\"\") )"
                    }
                },
                "rightChild": {
                    "nodeType": "Filter",
                    "child": {"nodeType": "LimitSkip", "child": {"nodeType": "Seek"}}
                }
            }
        }
    },
});
assertCount(result, numDuplicates, {c: [[1, 2, 3]]});
assertCount(result, numDuplicates, {c: [[1], [2], [3]]});
assertCount(result, numDuplicates, {c: [[2]]});
assert.eq(result.length, numDuplicates * 3);
})();
