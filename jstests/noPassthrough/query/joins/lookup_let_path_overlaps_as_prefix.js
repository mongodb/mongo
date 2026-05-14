/**
 * Repro for SERVER-126395: a $lookup whose `as` field shares a prefix with a local path referenced
 * through `let` in the sub-pipeline's $expr.
 *
 * Under join optimization, the optimizer adds the foreign node to the PathResolver before
 * resolving the `let` RHS. With `as: "X"` and `let: {l: "$X.y"}`, the local path "$X.y" is
 * misresolved as a foreign path, producing an equality predicate that connects the foreign node
 * to itself and triggering the "Self edges are not permitted" tassert. Without join optimization,
 * "$X.y" is correctly evaluated on the local document.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

TestData.cleanUpCoreDumpsFromExpectedCrash = true;
const conn = MongoRunner.runMongod({
    useLogFiles: false,
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnableJoinOptimization: true,
        internalEnablePathArrayness: true,
        internalJoinReorderMode: "bottomUp",
    },
});
assert(conn);
const testDB = conn.getDB(jsTestName());
const base = testDB.base;
const foreign = testDB.foreign;

// 'X.y' lives in the base collection. The join uses 'X.y' as the local side of the predicate and
// 'z' as the foreign side, while the $lookup overwrites 'X' via `as: "X"`.
assert.commandWorked(base.insert({_id: 0, X: {y: 1}}));
assert.commandWorked(foreign.insert({_id: "f", z: 1}));

// Indexes are required so the path-arrayness API can prove that `base.X.y` and `foreign.z` are
// scalar -- otherwise the optimizer bails out before the bug is reached.
assert.commandWorked(base.createIndex({"X.y": 1}));
assert.commandWorked(foreign.createIndex({z: 1}));

const pipeline = [
    {
        $lookup: {
            from: foreign.getName(),
            as: "X",
            let: {l: "$X.y"},
            pipeline: [{$match: {$expr: {$eq: ["$z", "$$l"]}}}],
        },
    },
    {$unwind: "$X"},
];

// Sanity check the naive plan: with join optimization off, "$X.y" resolves on the local document
// and the lookup matches `base.X.y` (1) against `foreign.z` (1).
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
const naive = base.aggregate(pipeline).toArray();
assert.eq(
    [{_id: 0, X: {_id: "f", z: 1}}],
    naive,
    "Naive $lookup must succeed and match base.X.y to foreign.z",
);

// With join optimization on, the optimizer must resolve "$X.y" against the local document, not the
// freshly-added foreign node. The expected behavior is that the optimized plan produces the same
// result as the naive plan.
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
const cmdRes = testDB.runCommand({aggregate: base.getName(), pipeline, cursor: {}});

// WILL FAIL pre-fix: 11180001 "Self edges are not permitted" tassert during join edge addition.
assert.commandWorked(cmdRes);
const optimized = new DBCommandCursor(testDB, cmdRes).toArray();
assert.eq(
    naive,
    optimized,
    "Optimized $lookup result must match the naive plan when `as` prefix overlaps a `let` path",
);

MongoRunner.stopMongod(conn, null, {allowedExitCode: MongoRunner.EXIT_ABORT});
