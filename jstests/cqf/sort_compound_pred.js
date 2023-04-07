(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sort_compound_pred;
t.drop();

const documents = [];
for (let i = 0; i < 100; i++) {
    for (let j = 0; j < 10; j++) {
        documents.push({a: i});
    }
}
assert.commandWorked(t.insertMany(documents));
assert.commandWorked(t.createIndex({a: 1}));

{
    const res = runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => t.explain("executionStats").aggregate([{$match: {$or: [{a: 1}, {a: 2}]}}]));
    assert.eq(20, res.executionStats.nReturned);

    // No collation node on the path (we are not sorting).
    assert.eq(removeUUIDsFromExplain(db, res),
              `Root [{scan_0}]
NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   Const [true]
|   LimitSkip [limit: 1, skip: 0]
|   Seek [ridProjection: rid_1, {'<root>': scan_0}, cqf_sort_compound_pred_]
SortedMerge []
|   |   |   collation: 
|   |   |       rid_1: Ascending
|   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sort_compound_pred_, indexDefName: a_1, interval: {=Const [2]}]
IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sort_compound_pred_, indexDefName: a_1, interval: {=Const [1]}]
`);
}

{
    const res = runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => t.explain("executionStats")
                  .aggregate([{$match: {$or: [{a: 1}, {a: 2}]}}, {$sort: {a: 1}}]));
    assert.eq(20, res.executionStats.nReturned);

    // Collation node on the path. It is not subsumed in the index scan because we have a compound
    // predicate.
    assert.eq(removeUUIDsFromExplain(db, res),
              `Root [{scan_0}]
Collation [{sort_0: Ascending}]
NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   Const [true]
|   LimitSkip [limit: 1, skip: 0]
|   Seek [ridProjection: rid_1, {'<root>': scan_0}, cqf_sort_compound_pred_]
GroupBy [{rid_1}]
|   aggregations: 
|       [sort_0]
|           FunctionCall [$first]
|           Variable [disjunction_0]
Union [{disjunction_0, rid_1}]
|   IndexScan [{'<indexKey> 0': disjunction_0, '<rid>': rid_1}, scanDefName: cqf_sort_compound_pred_, indexDefName: a_1, interval: {=Const [2]}]
IndexScan [{'<indexKey> 0': disjunction_0, '<rid>': rid_1}, scanDefName: cqf_sort_compound_pred_, indexDefName: a_1, interval: {=Const [1]}]
`);
}
}());
