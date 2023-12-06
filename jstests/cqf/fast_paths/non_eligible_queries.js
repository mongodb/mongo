/**
 * This test verifies that the optimizer fast path is not used for queries that would otherwise be
 * eligible but contain operations the current fast path implementations don't support (e.g. a
 * projection).
 */
import {isBonsaiFastPathPlan} from "jstests/libs/analyze_plan.js";
import {checkCascadesOptimizerEnabled} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the Bonsai optimizer is not enabled.");
    quit();
}

const coll = db.non_eligible_queries;
coll.drop();

{
    // Empty find with a projection should not use fast path.
    const explain = assert.commandWorked(coll.explain().find({}, {b: 1}).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Empty find with a sort spec should not use fast path.
    const explain = assert.commandWorked(coll.explain().find({}).sort({b: 1}).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Empty find with limit should not use fast path.
    const explain = assert.commandWorked(coll.explain().find({}).limit(3).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Empty find with skip should not use fast path.
    const explain = assert.commandWorked(coll.explain().find({}).skip(3).finish());
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Pipeline with $project should not use fast path.
    let explain =
        assert.commandWorked(coll.explain().aggregate([{$match: {}}, {$project: {a: 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain));

    explain = assert.commandWorked(coll.explain().aggregate([{$project: {a: 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Pipeline with $sort should not use fast path.
    let explain = assert.commandWorked(coll.explain().aggregate([{$match: {}}, {$sort: {a: 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain));

    explain = assert.commandWorked(coll.explain().aggregate([{$sort: {a: 1}}]));
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Pipeline with $limit should not use fast path.
    let explain = assert.commandWorked(coll.explain().aggregate([{$match: {}}, {$limit: 3}]));
    assert(!isBonsaiFastPathPlan(db, explain));

    explain = assert.commandWorked(coll.explain().aggregate([{$limit: 3}]));
    assert(!isBonsaiFastPathPlan(db, explain));
}
{
    // Pipeline with $skip should not use fast path.
    let explain = assert.commandWorked(coll.explain().aggregate([{$match: {}}, {$skip: 3}]));
    assert(!isBonsaiFastPathPlan(db, explain));

    explain = assert.commandWorked(coll.explain().aggregate([{$skip: 3}]));
    assert(!isBonsaiFastPathPlan(db, explain));
}
