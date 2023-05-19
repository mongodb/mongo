(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

const t = db.cqf_sorted_merge;
t.drop();

const index = {
    a: 1
};
assert.commandWorked(t.createIndex(index));

const query = {
    a: {$in: [0, 10, 20, 30]}
};

function getExplain() {
    return runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        () => t.explain().find(query).hint(index).sort({_id: 1}).finish());
}

function testCorrectness() {
    const resCollScan = t.find(query).hint({$natural: 1}).sort({_id: 1}).toArray();
    const resIndexScan = t.find(query).hint(index).sort({_id: 1}).toArray();
    assert.eq(resCollScan, resIndexScan);
}

// Non-multikey case.
assert.commandWorked(t.insert([{a: -1}, {a: 0}, {a: 0}, {a: 10}, {a: 21}, {a: 21}, {a: 30}]));
const nonMultikeyExplain =
    `Root [{scan_0}]
Collation [{sort_0: Ascending}]
NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   Const [true]
|   LimitSkip [limit: 1, skip: 0]
|   Seek [ridProjection: rid_1, {'<root>': scan_0, '_id': sort_0}, cqf_sorted_merge_]
SortedMerge []
|   |   |   |   |   collation: 
|   |   |   |   |       rid_1: Ascending
|   |   |   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [30]}]
|   |   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [20]}]
|   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [10]}]
IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [0]}]
`;
assert.eq(removeUUIDsFromExplain(db, getExplain()), nonMultikeyExplain);
testCorrectness();

// We should deduplicate RIDs in the multikey case.
assert.commandWorked(
    t.insert([{a: [0, 0]}, {a: [0, 5]}, {a: [8, 7]}, {a: [0, 20]}, {a: [10, 30]}, {a: []}]));
const multikeyExplain =
    `Root [{scan_0}]
Collation [{sort_0: Ascending}]
NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   Const [true]
|   LimitSkip [limit: 1, skip: 0]
|   Seek [ridProjection: rid_1, {'<root>': scan_0, '_id': sort_0}, cqf_sorted_merge_]
Unique [{rid_1}]
SortedMerge []
|   |   |   |   |   collation: 
|   |   |   |   |       rid_1: Ascending
|   |   |   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [30]}]
|   |   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [20]}]
|   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [10]}]
IndexScan [{'<rid>': rid_1}, scanDefName: cqf_sorted_merge_, indexDefName: a_1, interval: {=Const [0]}]
`;
assert.eq(removeUUIDsFromExplain(db, getExplain()), multikeyExplain);
testCorrectness();
}());
