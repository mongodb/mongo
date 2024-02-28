// Tests that a query with $in filter over a large array chooses the optimal index. We ban SBE
// multi planner to run as it chooses a plan with a suboptimal index.
import {getPlanStage} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

// The SBE multi planner chooses the wrong index. Remove this check when SBE runtime planners
// are deleted.
if (checkSbeFullyEnabled(db) &&
    !FeatureFlagUtil.isPresentAndEnabled(db, "ClassicRuntimePlanningForSbe")) {
    jsTestLog("Skipping test since SBE is enabled without classic runtime planning for SBE.");
    quit();
}

const coll = db.large_in_with_indexes;
coll.drop();

Random.setRandomSeed(1);

const docs = [];
const numDocs = 100000;
for (let i = 0; i < numDocs; i++) {
    docs.push({_id: i, rd: Random.randInt(numDocs), ard: Random.randInt(numDocs)});
}

assert.commandWorked(coll.createIndex({rd: 1}));
assert.commandWorked(coll.createIndex({ard: 1}));
assert.commandWorked(coll.createIndex({rd: 1, ard: 1}));
assert.commandWorked(coll.createIndex({_id: 1, rd: 1, ard: 1}));
assert.commandWorked(coll.insertMany(docs));

const inArray = [];

for (let i = 0; i < 300; i++) {
    inArray.push(Random.randInt(numDocs));
}

const explain =
    coll.find({rd: {$gte: 7}, _id: {$in: inArray}}).sort({ard: 1}).explain("executionStats");

const ixscanStage = getPlanStage(explain, "IXSCAN");
assert.eq(ixscanStage.indexName, "_id_1_rd_1_ard_1", ixscanStage);
