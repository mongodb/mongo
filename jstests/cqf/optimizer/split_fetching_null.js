import {
    checkCascadesOptimizerEnabled,
    removeUUIDsFromExplain,
    runWithParams
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const coll = db.cqf_split_fetching_null;
coll.drop();

// Prepare documents where only few of them have null/missing values whereas the rest of them have
// non-null values.
const docs = [
    {b: -1},
    {a: null, b: -2},
    {a: -1},
    {a: -2, b: null},
];

for (let i = 0; i < 1000; i++) {
    docs.push({a: 10, b: i * 100});
}

// Add extra docs to make sure indexes can be picked and 1/10 of them have a=10.
let newDocs = [];
for (let i = 0; i < 10000; i++) {
    newDocs.push({a: i, b: i * 10});
}

// Distribute interesting documents to encourage IndexScan when sampling in chunks.
for (let i = 0; i < docs.length; i++) {
    const idx = Math.floor(i * (newDocs.length / docs.length));
    newDocs[idx] = docs[i];
}

assert.commandWorked(coll.insertMany(newDocs));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

function runWithExplainV2(fn) {
    return runWithParams(
        [
            {key: 'internalCascadesOptimizerExplainVersion', value: "v2"},
            {key: "internalCascadesOptimizerUseDescriptiveVarNames", value: true}
        ],
        fn);
}

let res = runWithExplainV2(
    () =>
        coll.explain("executionStats").aggregate([{$match: {a: 10}}, {$project: {b: 1, _id: 0}}]));
assert.eq(1001, res.executionStats.nReturned);
// We expect to see a query plan which is a union of two index scans where one of them is without
// fetching documents. This plan is optimal given the distribution of the data in {[Const [10 |
// minKey], Const [10 | null]]} where there is only few of them require document fetching (i.e.
// IndexScan + Seek). So most of the documents will be handled by the covered IndexScan which is
// more performant than IndexScan + Seek.
const expectedStrEq =
    `Root [{combinedProjection_0}]
Evaluation [{combinedProjection_0}]
|   EvalPath []
|   |   Const [{}]
|   PathField [b]
|   PathConstant []
|   Variable [fieldProj_0]
Unique [{rid_1}]
Union [{fieldProj_0, rid_1}]
|   NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   |   Const [true]
|   |   LimitSkip [limit: 1, skip: 0]
|   |   Seek [ridProjection: rid_1, {'b': fieldProj_0}, cqf_split_fetching_null_]
|   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_split_fetching_null_, indexDefName: a_1_b_1, interval: {[Const [10 | minKey], Const [10 | null]]}]
IndexScan [{'<indexKey> 1': fieldProj_0, '<rid>': rid_1}, scanDefName: cqf_split_fetching_null_, indexDefName: a_1_b_1, interval: {(Const [10 | null], Const [10 | maxKey]]}]
`;
assert.eq(expectedStrEq, removeUUIDsFromExplain(db, res));
res = runWithExplainV2(() => coll.explain("executionStats").find({a: 10}, {b: 1, _id: 0}).finish());
assert.eq(1001, res.executionStats.nReturned);
assert.eq(expectedStrEq, removeUUIDsFromExplain(db, res));

res = runWithExplainV2(() => coll.explain("executionStats").aggregate([
    {$match: {$expr: {$lte: ['$a', 10]}}},
    {$project: {a: 1, _id: 0}}
]));
assert.eq(1013, res.executionStats.nReturned);
// We expect to see a query plan similar to the above. The difference is that the interval becomes
// an inequality and the field projection is 'a'.
const expectedStrLte =
    `Root [{combinedProjection_0}]
Evaluation [{combinedProjection_0}]
|   EvalPath []
|   |   Const [{}]
|   PathField [a]
|   PathConstant []
|   Variable [fieldProj_0]
Unique [{rid_1}]
Union [{fieldProj_0, rid_1}]
|   NestedLoopJoin [joinType: Inner, {rid_1}]
|   |   |   Const [true]
|   |   LimitSkip [limit: 1, skip: 0]
|   |   Seek [ridProjection: rid_1, {'a': fieldProj_0}, cqf_split_fetching_null_]
|   IndexScan [{'<rid>': rid_1}, scanDefName: cqf_split_fetching_null_, indexDefName: a_1_b_1, interval: {<=Const [null | maxKey]}]
IndexScan [{'<indexKey> 0': fieldProj_0, '<rid>': rid_1}, scanDefName: cqf_split_fetching_null_, indexDefName: a_1_b_1, interval: {(Const [null | maxKey], Const [10 | maxKey]]}]
`;
assert.eq(expectedStrLte, removeUUIDsFromExplain(db, res));
res = runWithExplainV2(
    () =>
        coll.explain("executionStats").find({$expr: {$lte: ['$a', 10]}}, {a: 1, _id: 0}).finish());
assert.eq(1013, res.executionStats.nReturned);
assert.eq(expectedStrLte, removeUUIDsFromExplain(db, res));
