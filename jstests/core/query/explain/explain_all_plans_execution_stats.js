// Verifies that execution stats are preserved in rejected plans in the SBE multiplanner's
// allPlansExecution verbosity.
//
// @tags: [
//   assumes_read_concern_local,
// ]
//
import {getExecutionStats} from "jstests/libs/analyze_plan.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";

if (checkSbeRestrictedOrFullyEnabled(db)) {
    const coll = db.test;
    assertDropCollection(coll.getDB(), coll.getName());
    assert.commandWorked(coll.createIndex({"a": 1}));
    assert.commandWorked(coll.createIndex({"b": 1}));
    assert.commandWorked(coll.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 1}, {a: 2, b: 2}]));
    const agg =
        [{$match: {a: 1, b: 1}}, {$group: {_id: null, sum_a: {$sum: "$a"}, sum_b: {$sum: "$b"}}}];

    const explainAllPlans = coll.explain("allPlansExecution").aggregate(agg);
    assert.commandWorked(explainAllPlans);
    const executionStatsAllShards = getExecutionStats(explainAllPlans);

    let examinedDoc = false;
    let examinedKey = false;
    let seenRejectedPlans = false;
    for (let i = 0; i < executionStatsAllShards.length; i++) {
        const rejectedPlans = executionStatsAllShards[i].allPlansExecution;
        for (let j = 0; j < rejectedPlans.length; j++) {
            seenRejectedPlans = true;
            if (rejectedPlans[j].totalDocsExamined > 0) {
                examinedDoc = true;
            }
            if (rejectedPlans[j].totalKeysExamined > 0) {
                examinedKey = true;
            }
        }
    }
    assert.eq(seenRejectedPlans, true, "did not see any rejected plans");
    assert.eq(examinedDoc, true, "did not examine any documents");
    assert.eq(examinedKey, true, "did not examine any keys");
}
