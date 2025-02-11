/**
 * Tests that or stages reports spilling metrics when explain is run with verbosities
 * "executionStats" and "allPlansExecution".
 *
 * @tags: [
 *   requires_fcv_81,
 *   # We modify the value of a query knob. setParameter is not persistent.
 *   does_not_support_stepdowns,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

if (!checkSbeCompletelyDisabled(db)) {
    jsTestLog("Skipping test because SBE is not disabled thus orStage is not used in the query");
    quit();
}

const coll = db.or_exec_stats;
coll.drop();

const memoryLimit = 128;  // Spill at 128B

assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

let bulk = coll.initializeUnorderedBulkOp();
const nDocs = 120;
for (let i = 0; i < nDocs; i++) {
    bulk.insert({_id: i, a: 1, b: 1});
}
assert.commandWorked(bulk.execute());

function setOrStageMemoryLimit(memoryLimit) {
    const commandResArr = FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            internalOrStageMaxMemoryBytes: memoryLimit,
        }
    });
    assert.gt(commandResArr.length, 0, "Setting memory limit failed");
    assert.commandWorked(commandResArr[0]);
}

function getOrStageStats(query, explainType) {
    let pipeline = [{"$match": query}, {"$group": {"_id": null, "n": {"$sum": 1}}}];
    let explain = coll.explain(explainType).aggregate(pipeline);
    let orStage = getAggPlanStages(explain, "OR");
    assert(orStage, `Plan does not have execution stats for or stage: ${tojson(explain)}`);

    return orStage;
}

let query = {$or: [{a: 1}, {b: 1}]};

// Check that the statistics exist but are zero, since the query did not spill.
assert.eq(nDocs, coll.countDocuments(query));

for (const explainType of ["executionStats", "allPlansExecution"]) {
    let orStage = getOrStageStats(query, explainType);

    for (let i = 0; i < orStage.length; ++i) {
        assert.eq(orStage[i].spills, 0);
        assert.eq(orStage[i].spilledBytes, 0);
        assert.eq(orStage[i].spilledRecords, 0);
        assert.eq(orStage[i].spilledDataStorageSize, 0);
    }
}

const oldMemoryLimit =
    assert.commandWorked(db.adminCommand({getParameter: 1, internalOrStageMaxMemoryBytes: 1}))
        .internalOrStageMaxMemoryBytes;

try {
    // Change the memory OR stage is allowed to use to cause spilling.
    setOrStageMemoryLimit(memoryLimit);
    assert.eq(nDocs, coll.countDocuments(query));

    // Check that the statistics exist and are not zero.
    for (const explainType of ["executionStats", "allPlansExecution"]) {
        let orStage = getOrStageStats(query, explainType);

        for (let i = 0; i < orStage.length; ++i) {
            assert.gt(orStage[i].spills, 0);
            assert.gt(orStage[i].spilledBytes, 0);
            assert.gt(orStage[i].spilledRecords, 0);
            assert.gt(orStage[i].spilledDataStorageSize, 0);
        }
    }

} finally {
    setOrStageMemoryLimit(oldMemoryLimit);
}
