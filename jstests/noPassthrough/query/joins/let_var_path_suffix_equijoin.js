/**
 * SERVER-126397: $lookup sub-pipeline references a `let` variable with a *path suffix* on the
 * variable, e.g. `$$l.y` where `let: {l: '$x'}`.
 *
 * Naive $lookup semantics: `$$l.y` evaluates the let variable to `base.x`, then traverses `.y`,
 * giving `base.x.y`. So the predicate `{$eq: ['$z', '$$l.y']}` becomes `foreign.z == base.x.y`.
 *
 * The join optimizer's predicate extractor (predicate_extractor.cpp, JoinPredicateExpr::make)
 * looks up the let variable's RHS but does NOT preserve the trailing path components used in
 * the sub-pipeline. So `$$l.y` is treated as `$$l`, producing the predicate
 * `foreign.z == base.x` (an Object) which never matches the scalar `foreign.z`.
 *
 * Without join optimization the naive plan correctly evaluates to `base.x.y`, finds the matching
 * foreign document, and returns it. With the optimizer, the matching foreign document is wrongly
 * filtered out and the result set is empty.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

const conn = MongoRunner.runMongod({
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
assert.commandWorked(base.insert({_id: 0, x: {y: 5}}));
assert.commandWorked(foreign.insert({_id: "f", z: 5}));

// Indexes on the scalar paths used in the join predicate so the optimizer's path-arrayness check
// proves both sides are scalar and join optimization proceeds.
assert.commandWorked(base.createIndex({"x.y": 1}));
assert.commandWorked(base.createIndex({"x": 1}));
assert.commandWorked(foreign.createIndex({z: 1}));

const pipeline = [
    {
        $lookup: {
            from: foreign.getName(),
            as: "foreign",
            let: {l: "$x"},
            pipeline: [{$match: {$expr: {$eq: ["$z", "$$l.y"]}}}],
        },
    },
    {$unwind: "$foreign"},
    {$project: {_id: 1, "foreign._id": 1}},
];

assert.commandWorked(testDB.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
const naive = base.aggregate(pipeline).toArray();
assert.eq(
    [{_id: 0, foreign: {_id: "f"}}],
    naive,
    "Naive $lookup should evaluate $$l.y as base.x.y and match foreign.z",
);

// WILL RETURN WRONG RESULT: []
assert.commandWorked(testDB.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
const optimized = base.aggregate(pipeline).toArray();
assert.sameMembers(
    optimized,
    naive,
    "Join optimizer dropped the '.y' suffix on the let variable, returning wrong results",
);

MongoRunner.stopMongod(conn);
