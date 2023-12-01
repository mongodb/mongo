/**
 * Tests for optimizations applied to trivially false predicates.
 * @tags: [
 *   requires_fcv_73,
 *   # Explain command does not support read concerns other than local
 *   assumes_read_concern_local
 * ]
 */

import {getWinningPlanFromExplain, isEofPlan} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js"

const collName = "jstests_explain_find_trivially_false_predicates";

[{description: "Regular collections", collOptions: {}}, {
    description: "Clustered collections",
    collOptions:
        {clusteredIndex: {key: {_id: 1}, unique: true, name: "Clustered index definition"}},
}].forEach((testConfig) => {
    jsTestLog(`Testing trivially false optimization with ${testConfig.description}`);
    assertDropAndRecreateCollection(db, collName, testConfig.collOptions);
    const coll = db[collName];

    assert.commandWorked(coll.insert(Array.from({length: 10}, (_, i) => ({_id: i, a: i}))));

    // Finding something trivially false (e.g: alwaysFalse) is optimized using an EOF plan.
    let explain = coll.find({$alwaysFalse: 1}).explain();
    let winningPlan = getWinningPlanFromExplain(explain);
    assert(isEofPlan(db, winningPlan));

    // It also uses EOF for queries including projection, sorting, limit and skip arguments.
    explain = coll.find({$alwaysFalse: 1}, {_id: 0, a: 1}).skip(1).limit(2).explain();
    winningPlan = getWinningPlanFromExplain(explain);
    assert(isEofPlan(db, winningPlan));
});
