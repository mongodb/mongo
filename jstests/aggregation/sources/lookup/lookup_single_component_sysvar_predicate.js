/**
 * Regression: assertion when a single-component system variable (e.g. $$NOW, $$ROOT, $$CURRENT)
 * is used inside a $lookup join predicate ($expr in the sub-pipeline's $match, or in a downstream
 * $match between two $lookups that the join optimizer attempts to push down).
 *
 * Root cause: extractExpressionCompare() in predicate_extractor.cpp unconditionally calls
 * ExpressionFieldPath::getFieldPathWithoutCurrentPrefix() on both sides of a $eq once they are
 * confirmed to be ExpressionFieldPath pointers. That helper invokes FieldPath::tail(), which trips
 * tassert(16409, "FieldPath::tail() called on single element path", getPathLength() > 1) when the
 * referenced path has a single component. Bare system variables ($$NOW, $$ROOT, $$CURRENT) are
 * stored as ExpressionFieldPath whose underlying _fieldPath is a single component ("NOW", "ROOT",
 * "CURRENT"). $$NOW is preserved as an ExpressionFieldPath (not constant-folded) unless
 * featureFlagSbeFull is enabled, so it reaches the join optimizer intact.
 *
 * The query should either return an expected (typically empty) result or be rejected with a clean
 * error -- it must not crash the server with a tassert.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
const testDB = db.getSiblingDB(jsTestName());
const base = testDB.base;
const left = testDB.left;
const right = testDB.right;

base.drop();
left.drop();
right.drop();

assert.commandWorked(base.insert({_id: 0, lk: 1, rk: 1, ts: new Date(0)}));
assert.commandWorked(base.insert({_id: 1, lk: 2, rk: 2, ts: new Date(0)}));
assert.commandWorked(left.insert({_id: "left", lk: 1, a: 1}));
assert.commandWorked(right.insert({_id: "right", rk: 1, b: 1, ts: new Date(0)}));

assert.commandWorked(base.createIndex({lk: 1, rk: 1}));
assert.commandWorked(left.createIndex({lk: 1}));
assert.commandWorked(right.createIndex({rk: 1}));

// Helper: a command either runs cleanly (returns its result array) or fails with a non-crash
// error code. Either is acceptable; an unclean shutdown / tassert is not.
function runOrAcceptError(coll, pipeline, label) {
    const cmdRes = testDB.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}});
    if (cmdRes.ok === 1) {
        jsTestLog(label + ": ok, cursor returned");
        return new DBCommandCursor(testDB, cmdRes).toArray();
    }
    // Acceptable: any clean error code. Unacceptable: tassert / crash (caught by parallel suite).
    jsTestLog(label + ": clean error, code=" + cmdRes.code + " errmsg=" + cmdRes.errmsg);
    return null;
}

// Case 1: $$NOW in a join predicate between two $lookups. This reproduces the original ticket.
// The $eq compares the single-component system variable $$NOW (a Date) against a scalar from the
// upstream left $lookup -- the equality can never hold but the optimizer must not crash trying to
// extract it as a join predicate.
{
    const pipeline = [
        {$lookup: {from: left.getName(), localField: "lk", foreignField: "lk", as: "left"}},
        {$unwind: "$left"},
        {$lookup: {from: right.getName(), localField: "rk", foreignField: "rk", as: "right"}},
        {$unwind: "$right"},
        {$match: {$expr: {$eq: ["$$NOW", "$left.a"]}}},
    ];
    const out = runOrAcceptError(base, pipeline, "case1 $$NOW vs $left.a");
    if (out !== null) {
        assert.eq([], out, "Date ($$NOW) is never equal to scalar 1");
    }
}

// Case 2: $$NOW inside a $lookup let + sub-pipeline. let binds $$now to a foreign timestamp; the
// sub-pipeline predicate equates $$NOW (the bare system variable, single-component) against the
// let-bound variable. The join optimizer should not attempt -- or, attempting, must not crash --
// to push this down as an equijoin.
{
    const pipeline = [
        {
            $lookup: {
                from: right.getName(),
                let: {ts: "$ts"},
                pipeline: [{$match: {$expr: {$eq: ["$$NOW", "$$ts"]}}}],
                as: "matched",
            },
        },
    ];
    const out = runOrAcceptError(base, pipeline, "case2 $$NOW in let + sub-pipeline");
    if (out !== null) {
        // $$NOW is the wall-clock at query time; the seeded $ts is epoch. They never match, so the
        // 'matched' array must be empty for every base doc.
        for (const doc of out) {
            assert.eq([], doc.matched, "matched array must be empty: " + tojson(doc));
        }
    }
}

// Case 3: $$ROOT used in a downstream $expr equality. $$ROOT is also single-component and exercises
// the same code path.
{
    const pipeline = [
        {$lookup: {from: left.getName(), localField: "lk", foreignField: "lk", as: "left"}},
        {$unwind: "$left"},
        {$match: {$expr: {$eq: ["$$ROOT", "$left"]}}},
    ];
    runOrAcceptError(base, pipeline, "case3 $$ROOT vs $left");
}

// Case 4: $$CURRENT mirror. By default $$CURRENT aliases $$ROOT, so the same single-component
// path-tail code path is exercised.
{
    const pipeline = [
        {$lookup: {from: left.getName(), localField: "lk", foreignField: "lk", as: "left"}},
        {$unwind: "$left"},
        {$match: {$expr: {$eq: ["$$CURRENT", "$left"]}}},
    ];
    runOrAcceptError(base, pipeline, "case4 $$CURRENT vs $left");
}

// Case 5: confirm the server is still healthy after running every pipeline above. If any of them
// had crashed the server we would have failed before reaching this point; this is a belt-and-
// braces final ping.
assert.commandWorked(testDB.runCommand({ping: 1}));
