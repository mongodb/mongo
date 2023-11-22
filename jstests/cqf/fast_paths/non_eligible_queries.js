/**
 * This test verifies that the optimizer fast path is not used for queries that would otherwise be
 * eligible but contain operations the current fast path implementations don't support (e.g. a
 * projection).
 */
import {planHasStage} from "jstests/libs/analyze_plan.js";
import {
    checkCascadesOptimizerEnabled,
    checkFastPathEnabled,
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

if (!checkFastPathEnabled(db)) {
    jsTestLog("Skipping test because fast paths are not enabled");
    quit();
}

function assertNotUsingFastPath(explainCmd) {
    const explain = assert.commandWorked(explainCmd);
    assert(!planHasStage(db, explain, "FASTPATH"));
}

const coll = db.non_eligible_queries;
coll.drop();

{
    // Empty find with a projection should not use fast path.
    const explain = coll.explain().find({}, {b: 1}).finish();
    assertNotUsingFastPath(explain);
}
{
    // Empty find with a sort spec should not use fast path.
    const explain = coll.explain().find({}).sort({b: 1}).finish();
    assertNotUsingFastPath(explain);
}
{
    // Empty find with limit should not use fast path.
    const explain = coll.explain().find({}).limit(3).finish();
    assertNotUsingFastPath(explain);
}
{
    // Empty find with skip should not use fast path.
    const explain = coll.explain().find({}).skip(3).finish();
    assertNotUsingFastPath(explain);
}
{
    // Pipeline with $project should not use fast path.
    let explain = coll.explain().aggregate([{$match: {}}, {$project: {a: 1}}]);
    assertNotUsingFastPath(explain);

    explain = coll.explain().aggregate([{$project: {a: 1}}]);
    assertNotUsingFastPath(explain);
}
{
    // Pipeline with $sort should not use fast path.
    let explain = coll.explain().aggregate([{$match: {}}, {$sort: {a: 1}}]);
    assertNotUsingFastPath(explain);

    explain = coll.explain().aggregate([{$sort: {a: 1}}]);
    assertNotUsingFastPath(explain);
}
{
    // Pipeline with $limit should not use fast path.
    let explain = coll.explain().aggregate([{$match: {}}, {$limit: 3}]);
    assertNotUsingFastPath(explain);

    explain = coll.explain().aggregate([{$limit: 3}]);
    assertNotUsingFastPath(explain);
}
{
    // Pipeline with $skip should not use fast path.
    let explain = coll.explain().aggregate([{$match: {}}, {$skip: 3}]);
    assertNotUsingFastPath(explain);

    explain = coll.explain().aggregate([{$skip: 3}]);
    assertNotUsingFastPath(explain);
}
