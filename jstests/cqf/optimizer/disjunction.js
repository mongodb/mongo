/**
 * Test that $or is translated to a SargableNode, and executed with correct results.
 */
(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const coll = db.cqf_disjunction;
coll.drop();

let docs = [];
for (let i = 0; i < 10; ++i) {
    // Generate enough documents for an index to be preferable.
    for (let a = 0; a < 10; ++a) {
        for (let b = 0; b < 10; ++b) {
            docs.push({a, b});
        }
    }
}
assert.commandWorked(coll.insert(docs));

let result = coll.find({$or: [{a: 2}, {b: 3}]}).toArray();
assert.eq(result.length, 190, result);
for (const doc of result) {
    assert(doc.a === 2 || doc.b === 3, "Query returned a doc not matching the predicate: ${doc}");
}

assert.commandWorked(coll.createIndexes([
    {a: 1},
    {b: 1},
]));

result = coll.find({$or: [{a: 2}, {b: 3}]}).toArray();
assert.eq(result.length, 190, result);
for (const doc of result) {
    assert(doc.a === 2 || doc.b === 3, "Query returned a doc not matching the predicate: ${doc}");
}

// At time of writing, queries that compare to literal array or MinKey/MaxKey are translated to
// an ABT with a disjunction in it.
result = coll.find({arr: {$eq: [2]}}).toArray();
assert.eq(result.length, 0, result);

result = coll.find({arr: {$gt: MinKey()}}).toArray();
assert.eq(result.length, docs.length, result);

// Test a nested or/and where one leaf predicate ($exists) cannot be fully satisfied with index
// bounds.
result = coll.find({
                 $or: [
                     // 'b' exists on every doc so this should never happen.
                     {a: 5, b: {$exists: false}},
                     // The field 'nope' doesn't exist so this also shouldn't happen.
                     {nope: 'nope'},
                 ]
             })
             .toArray();
assert.eq(result.length, 0, result);

// Test that adding an $or predicate doesn't inhibit the use of index scan for other predicates.
// The $or can just be a residual predicate.
{
    const res = runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => coll.explain("executionStats")
                  .find({a: 2, $or: [{b: 2}, {no_such_field: 123}]})
                  .finish());
    assert.eq(10, res.executionStats.nReturned);

    // We get an index scan on 'a' and some expression for the $or.
    const expectedStr =
        `Root [{scan_0}]
Filter []
|   BinaryOp [Or]
|   |   EvalFilter []
|   |   |   Variable [scan_0]
|   |   PathGet [no_such_field]
|   |   PathTraverse [1]
|   |   PathCompare [Eq]
|   |   Const [123]
|   EvalFilter []
|   |   Variable [scan_0]
|   PathGet [b]
|   PathCompare [Eq]
|   Const [2]
NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   Const [true]
|   LimitSkip [limit: 1, skip: 0]
|   Seek [ridProjection: rid_1, {'<root>': scan_0}, cqf_disjunction_]
IndexScan [{'<rid>': rid_1}, scanDefName: cqf_disjunction_, indexDefName: a_1, interval: {=Const [2]}]
`;
    const actualStr = removeUUIDsFromExplain(db, res);
    assert.eq(expectedStr, actualStr);
}
}());
