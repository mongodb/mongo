/**
 * Verify that the solutionHashUnstable of a winning plan can be used in forcedPlanSolutionHash,
 * even after the winning solution has been extended.
 */
import {getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryAllowForcedPlanByHash: true,
        featureFlagSbeFull: true,
    },
});
assert.neq(conn, null);
const db = conn.getDB("test");
const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
const docs = [];
for (let i = 0; i < 50; i++) {
    docs.push({a: i, b: i % 5, c: i % 7});
}
assert.commandWorked(coll.insertMany(docs));

const winningHashOf = (explain) => {
    const wp = getWinningPlanFromExplain(explain);
    return wp.solutionHashUnstable ?? wp.queryPlan?.solutionHashUnstable;
};

// Extension of the winning solution during deferred engine selection is observable in explain: the
// pushed-down $group only appears as a GROUP stage in the winning plan if the solution was
// extended. Note that the reported engine is not a usable signal here, as with featureFlagSbeFull
// even an unextended plain find runs on SBE.
function assertExtended(label, explain, expectExtended) {
    const extended = planHasStage(db, getWinningPlanFromExplain(explain), "GROUP");
    assert.eq(expectExtended, extended, label + ": unexpected winning plan extension", {explain});
}

function verify(label, baseCmd, expectExtended) {
    const explain = assert.commandWorked(
        db.runCommand({explain: baseCmd, verbosity: "queryPlanner"}),
    );
    const winHash = winningHashOf(explain);
    assert(winHash !== undefined, label + ": no winning solutionHashUnstable: " + tojson(explain));
    assertExtended(label, explain, expectExtended);

    const forced = db.runCommand({...baseCmd, forcedPlanSolutionHash: winHash});
    assert.commandWorked(forced, label + ": forcing the winning plan's reported hash failed");

    // Also verify that explain with a forced hash also reports the exact same hash.
    const forcedExplain = assert.commandWorked(
        db.runCommand({
            explain: {...baseCmd, forcedPlanSolutionHash: winHash},
            verbosity: "queryPlanner",
        }),
    );
    const forcedWinHash = winningHashOf(forcedExplain);
    assertExtended(label + " (forced)", forcedExplain, expectExtended);
    assert.eq(
        String(winHash),
        String(forcedWinHash),
        label + ": Winning plan hash doesn't match the forced hash",
    );
}

// Plain find. no pushdown so winning solution not extended.
verify("find", {find: coll.getName(), filter: {a: {$gte: 0}}, sort: {b: 1}}, false);

// $group pushed down to SBE, so the winning solution IS extended during deferred engine selection.
verify(
    "agg",
    {
        aggregate: coll.getName(),
        pipeline: [{$match: {a: {$gte: 0}}}, {$group: {_id: "$b", n: {$sum: 1}}}],
        cursor: {},
    },
    true,
);

MongoRunner.stopMongod(conn);
