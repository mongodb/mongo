// Verifies that execution stats are preserved in rejected plans in the SBE multiplanner's
// allPlansExecution verbosity.
//
// @tags: [
//   assumes_read_concern_local,
// ]
//
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {getExecutionStats} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";

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

    let seenRejectedPlans = false;
    assert(executionStatsAllShards.length > 0);
    for (let i = 0; i < executionStatsAllShards.length; i++) {
        assert(executionStatsAllShards[i].hasOwnProperty("allPlansExecution"));
        assert(executionStatsAllShards[i].hasOwnProperty("totalKeysExamined"));
        assert(executionStatsAllShards[i].hasOwnProperty("totalDocsExamined"));
        const rejectedPlans = executionStatsAllShards[i].allPlansExecution;
        for (let j = 0; j < rejectedPlans.length; j++) {
            seenRejectedPlans = true;
            if (executionStatsAllShards[i].totalDocsExamined) {
                assert(rejectedPlans[j].totalDocsExamined > 0,
                       "did not examine any documents " + tojson(rejectedPlans[j]));
            }
            if (executionStatsAllShards[i].totalKeysExamined) {
                assert(rejectedPlans[j].totalKeysExamined > 0,
                       "did not examine any keys" + tojson(rejectedPlans[j]));
            }
        }
    }
    assert.eq(seenRejectedPlans, true, "did not see any rejected plans");
}
