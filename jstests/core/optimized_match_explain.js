// @tags: [does_not_support_stepdowns]

/**
 * Tests that the explain output for $match reflects any optimizations.
 */
(function() {
    "use strict";
    load("jstests/libs/analyze_plan.js");

    const coll = db.match_explain;
    coll.drop();

    assert.writeOK(coll.insert({a: 1, b: 1}));
    assert.writeOK(coll.insert({a: 2, b: 3}));
    assert.writeOK(coll.insert({a: 1, b: 2}));
    assert.writeOK(coll.insert({a: 1, b: 4}));

    // Explain output should reflect optimizations.
    // $and should not be in the explain output because it is optimized out.
    let explain = coll.explain().aggregate(
        [{$sort: {b: -1}}, {$addFields: {c: {$mod: ["$a", 4]}}}, {$match: {$and: [{c: 1}]}}]);

    assert.commandWorked(explain);
    assert.eq(getAggPlanStage(explain, "$match"), {$match: {c: {$eq: 1}}});
}());
