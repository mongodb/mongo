/**
 * SERVER-126392: Post-$lookup $match arrayness is not checked.
 *
 * Repro for a join-optimization crash on a post-$lookup $match.
 *
 * The $lookup join keys are scalar, but the later $match compares dotted paths that traverse
 * arrays in the joined documents. Join optimization adds that $match equality to the join graph
 * without rechecking path arrayness, so CE treats the paths as scalar and trips the NDV
 * tassert (11158502 "Encountered unexpected array in NDV computation").
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

TestData.cleanUpCoreDumpsFromExpectedCrash = true;
const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnableJoinOptimization: true,
        internalEnablePathArrayness: true,
        internalJoinReorderMode: "bottomUp",
        internalJoinMethod: "HJ",
    },
});
assert(conn);
const testDB = conn.getDB(jsTestName());
const base = testDB.base;
const left = testDB.left;
const right = testDB.right;

assert.commandWorked(base.insertOne({_id: 0, lk: 1, rk: 1}));
assert.commandWorked(left.insertOne({_id: "left", lk: 1, arr: [{v: 1}]}));
assert.commandWorked(right.insertOne({_id: "right", rk: 1, arr: [{v: 1}]}));

// Indexes are required so the path-arrayness API can prove the lookup join keys are scalar.
// Without them, joinopt is skipped on the conservative side, hiding the bug.
assert.commandWorked(base.createIndex({lk: 1, rk: 1}));
assert.commandWorked(left.createIndex({lk: 1}));
assert.commandWorked(right.createIndex({rk: 1}));

const pipeline = [
    {$lookup: {from: left.getName(), localField: "lk", foreignField: "lk", as: "left"}},
    {$unwind: "$left"},
    {$lookup: {from: right.getName(), localField: "rk", foreignField: "rk", as: "right"}},
    {$unwind: "$right"},
    // The join keys (lk, rk) are scalar; arrayness on left/right is fine for joinopt to fire.
    // But this post-$lookup $match references dotted paths that traverse arrays in the joined
    // docs: left.arr.v and right.arr.v are both array-valued. Join optimization should detect
    // this and either bail out of joinopt or treat these paths as multikey in CE.
    {$match: {$expr: {$eq: ["$left.arr.v", "$right.arr.v"]}}},
    {$project: {_id: 0, "left._id": 1, "right._id": 1}},
];

// Sanity check the naive plan (joinopt off): the naive $lookup pipeline returns the join.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
const naive = base.aggregate(pipeline).toArray();
assert.eq(
    [{left: {_id: "left"}, right: {_id: "right"}}],
    naive,
    "Naive $lookup should match the two dotted paths through arrays",
);

// With join optimization on, the post-$lookup $match equality is folded into the join graph
// without rechecking arrayness on left.arr.v / right.arr.v. CE then tries to compute NDV on
// what it believes are scalar paths and trips a tassert.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
const cmdRes = testDB.runCommand({aggregate: base.getName(), pipeline, cursor: {}});

// Currently this assertion fails: tassert 11158502 "Encountered unexpected array in NDV
// computation" fires. Once SERVER-126392 lands, the aggregation should either succeed with the
// same result as the naive plan or bail out of join optimization for this prefix.
assert.commandWorked(cmdRes);
if (cmdRes.ok) {
    const opt = new DBCommandCursor(testDB, cmdRes).toArray();
    assert.eq(
        [{left: {_id: "left"}, right: {_id: "right"}}],
        opt,
        "Optimized plan must agree with naive plan once arrayness is rechecked",
    );
}

MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABORT});
