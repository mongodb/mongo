/**
 * Test that $or is translated to a SargableNode, and executed with correct results.
 */
import {
    checkCascadesOptimizerEnabled,
    removeUUIDsFromExplain,
    runWithFastPathsDisabled,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const coll = db.cqf_disjunction;
coll.drop();

let docs = [];
// Generate 100 documents with different pairs of a,b values.
for (let i = 0; i < 10; ++i) {
    for (let a = 0; a < 10; ++a) {
        for (let b = 0; b < 10; ++b) {
            docs.push({a, b});
        }
    }
}
// Generate extra non-matching documents to discourage collection scan.
for (let i = 0; i < 1000; ++i) {
    docs.push({});
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

{
    const query = {$or: [{a: 2}, {b: 3}]};

    // Check the plan and count.
    const res = runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => coll.explain("executionStats").find(query).finish());
    assert.eq(190, res.executionStats.nReturned);

    // We should get a union of two indexes {a:1} and {b:1}.
    const expectedStr =
        `Root [{scan_0}]
NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   Const [true]
|   LimitSkip [limit: 1, skip: 0]
|   Seek [ridProjection: rid_1, {'<root>': scan_0}, cqf_disjunction_]
Unique [{rid_1}]
Union [{rid_1}]
|   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_disjunction_, indexDefName: b_1, interval: {=Const [3]}]
IndexScan [{'<rid>': rid_1}, scanDefName: cqf_disjunction_, indexDefName: a_1, interval: {=Const [2]}]
`;
    const actualStr = removeUUIDsFromExplain(db, res);
    assert.eq(expectedStr, actualStr);

    // Check the full result.
    const result = coll.find(query).toArray();
    assert.eq(result.length, 190, result);
    for (const doc of result) {
        assert(doc.a === 2 || doc.b === 3,
               "Query returned a doc not matching the predicate: ${doc}");
    }
}

// At time of writing, queries that compare to literal array or MinKey/MaxKey are translated to
// an ABT with a disjunction in it.
result = coll.find({arr: {$eq: [2]}}).toArray();
assert.eq(result.length, 0, result);

// See SERVER-68274.
result = runWithFastPathsDisabled(() => coll.find({arr: {$gt: MinKey()}}).toArray());
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

// Test that an $or containing multiple predicates on the same field groups the predicates under
// the shared field.
{
    const params = [
        {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
        {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true},
        {key: "internalCascadesOptimizerDisableIndexes", value: true},
        // TODO SERVER-83574: Enable after implementing rewriting single-field disjunctions to
        // eqMember.
        {key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false}
    ];

    //
    // Test $or where all predicates are on the same field.
    //
    let res = runWithParams(
        params,
        () => coll.explain("executionStats").find({$or: [{a: 1}, {a: 2}, {a: 3}]}).finish());

    let expectedStr =
        `Root [{scan_0}]
Filter []
|   EvalFilter []
|   |   Variable [evalTemp_0]
|   PathTraverse [1]
|   PathCompare [EqMember]
|   Const [[1, 2, 3]]
PhysicalScan [{'<root>': scan_0, 'a': evalTemp_0}, cqf_disjunction_]
`;
    assert.eq(300, res.executionStats.nReturned);
    let actualStr = removeUUIDsFromExplain(db, res);
    assert.eq(expectedStr, actualStr);

    // The same query, but with nested $ors.
    res = runWithParams(
        params,
        () =>
            coll.explain("executionStats").find({$or: [{$or: [{a: 1}, {a: 2}, {a: 3}]}]}).finish());
    assert.eq(300, res.executionStats.nReturned);
    assert.eq(expectedStr, actualStr);

    res = runWithParams(
        params,
        () =>
            coll.explain("executionStats").find({$or: [{a: 1}, {$or: [{a: 2}, {a: 3}]}]}).finish());
    assert.eq(300, res.executionStats.nReturned);
    assert.eq(expectedStr, actualStr);

    //
    // Test $or where two predicates are on the same field and one is on a different field.
    //
    res = runWithParams(
        params,
        () => coll.explain("executionStats").find({$or: [{a: 1}, {a: 2}, {b: 3}]}).finish());

    expectedStr =
        `Root [{scan_0}]
Filter []
|   BinaryOp [Or]
|   |   EvalFilter []
|   |   |   Variable [evalTemp_1]
|   |   PathTraverse [1]
|   |   PathCompare [Eq]
|   |   Const [3]
|   EvalFilter []
|   |   Variable [evalTemp_0]
|   PathTraverse [1]
|   PathCompare [EqMember]
|   Const [[1, 2]]
PhysicalScan [{'<root>': scan_0, 'a': evalTemp_0, 'b': evalTemp_1}, cqf_disjunction_]
`;
    assert.eq(280, res.executionStats.nReturned);
    actualStr = removeUUIDsFromExplain(db, res);
    assert.eq(expectedStr, actualStr);

    // The same query, but with nested $ors.
    res = runWithParams(
        params,
        () =>
            coll.explain("executionStats").find({$or: [{$or: [{a: 1}, {a: 2}]}, {b: 3}]}).finish());
    assert.eq(280, res.executionStats.nReturned);
    assert.eq(expectedStr, actualStr);

    res = runWithParams(
        params,
        () =>
            coll.explain("executionStats").find({$or: [{$or: [{a: 1}, {b: 3}]}, {a: 2}]}).finish());
    assert.eq(280, res.executionStats.nReturned);
    assert.eq(expectedStr, actualStr);

    //
    // Test $or where two predicates are on one field and two predicates are on another.
    //
    res = runWithParams(
        params,
        () =>
            coll.explain("executionStats").find({$or: [{a: 1}, {a: 2}, {b: 3}, {b: 4}]}).finish());

    expectedStr =
        `Root [{scan_0}]
Filter []
|   BinaryOp [Or]
|   |   EvalFilter []
|   |   |   Variable [evalTemp_1]
|   |   PathTraverse [1]
|   |   PathCompare [EqMember]
|   |   Const [[3, 4]]
|   EvalFilter []
|   |   Variable [evalTemp_0]
|   PathTraverse [1]
|   PathCompare [EqMember]
|   Const [[1, 2]]
PhysicalScan [{'<root>': scan_0, 'a': evalTemp_0, 'b': evalTemp_1}, cqf_disjunction_]
`;
    assert.eq(360, res.executionStats.nReturned);
    actualStr = removeUUIDsFromExplain(db, res);
    assert.eq(expectedStr, actualStr);

    // The same query, but with nested $ors.
    runWithParams(params,
                  () => coll.explain("executionStats")
                            .find({$or: [{$or: [{a: 1}, {a: 2}]}, {$or: [{b: 3}, {b: 4}]}]})
                            .finish());
    assert.eq(360, res.executionStats.nReturned);
    assert.eq(expectedStr, actualStr);

    runWithParams(params,
                  () => coll.explain("executionStats")
                            .find({$or: [{$or: [{a: 1}, {b: 4}]}, {$or: [{b: 3}, {a: 2}]}]})
                            .finish());
    assert.eq(360, res.executionStats.nReturned);
    assert.eq(expectedStr, actualStr);
}

// Test a union involving multikey indexes.
// First make {a:1} and {b:1} multikey.
assert.commandWorked(coll.insert({a: ['asdf'], b: ['qwer']}));
{
    const query = {$or: [{a: 2}, {b: 3}]};

    // Check the plan and count.
    const res = runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => coll.explain("executionStats").find(query).finish());
    assert.eq(190, res.executionStats.nReturned);

    // We should get a union of two indexes {a:1} and {b:1}.
    // Neither one needs its own Unique stage, because we have to have a Unique after the Union
    // anyway.
    const expectedStr =
        `Root [{scan_0}]
NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   Const [true]
|   LimitSkip [limit: 1, skip: 0]
|   Seek [ridProjection: rid_1, {'<root>': scan_0}, cqf_disjunction_]
Unique [{rid_1}]
Union [{rid_1}]
|   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_disjunction_, indexDefName: b_1, interval: {=Const [3]}]
IndexScan [{'<rid>': rid_1}, scanDefName: cqf_disjunction_, indexDefName: a_1, interval: {=Const [2]}]
`;
    const actualStr = removeUUIDsFromExplain(db, res);
    assert.eq(expectedStr, actualStr);

    // Check the full result.
    const result = coll.find(query).toArray();
    assert.eq(result.length, 190, result);
    for (const doc of result) {
        assert(doc.a === 2 || doc.b === 3,
               "Query returned a doc not matching the predicate: ${doc}");
    }
}
