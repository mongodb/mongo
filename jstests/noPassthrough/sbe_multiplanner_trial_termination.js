/**
 * Tests the logic around the termination condition for the SBE multiplanner. In particular,
 * demonstrates that unlike the classic multiplanner, the SBE multiplanner's end condition is by
 * default not proportional to the size of the collection.
 */
(function() {
"use strict";

const numDocs = 1000;
const dbName = "sbe_multiplanner_db";
const collName = "sbe_multiplanner_coll";
const collFracKnob = "internalQueryPlanEvaluationCollFraction";
const collFracKnobSbe = "internalQueryPlanEvaluationCollFractionSbe";
const worksKnob = "internalQueryPlanEvaluationWorks";

const defaultCollFrac = 0.3;
const trialLengthFromCollFrac = defaultCollFrac * numDocs;
const trialLengthFromWorksKnob = 0.1 * numDocs;

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(dbName);
const coll = db[collName];

// Gets the "allPlansExecution" section from the explain of a query that has zero results, but for
// which the only two available indexed plans are highly unselective.
//
// Also asserts that the explain has the given version number.
function getAllPlansExecution(explainVersion) {
    const explain = coll.find({a: 1, b: 1, c: 1}).explain("allPlansExecution");
    assert.eq(explain.explainVersion, explainVersion, explain);
    assert(explain.hasOwnProperty("executionStats"), explain);
    const execStats = explain.executionStats;
    assert(execStats.hasOwnProperty("allPlansExecution"), explain);
    return execStats.allPlansExecution;
}

// Create a collection with two indices, where neither index is helpful in answering the query.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
for (let i = 0; i < numDocs; ++i) {
    assert.commandWorked(coll.insert({a: 1, b: 1}));
}

// Lower the value of the 'internalQueryPlanEvaluationWorks' so that it is smaller than 30% of the
// collection. Since the classic multiplanner takes either the works limit or 30% of the collection
// size -- whichever is larger -- this should cause the trial period to run for about 0.3 * numDocs
// work cycles.
const getParamRes = assert.commandWorked(db.adminCommand({getParameter: 1, [collFracKnob]: 1}));
assert.eq(getParamRes[collFracKnob], defaultCollFrac);
assert.commandWorked(db.adminCommand({setParameter: 1, [worksKnob]: trialLengthFromWorksKnob}));

// Force the classic engine and run an "allPlansExecution" verbosity explain. Confirm that the trial
// period terminates based on the the "collection fraction" as opposed to
// 'internalQueryPlanEvaluationWorks'.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));
let allPlans = getAllPlansExecution("1");
for (let plan of allPlans) {
    assert(plan.hasOwnProperty("executionStages"), plan);
    const executionStages = plan.executionStages;
    assert(executionStages.hasOwnProperty("works"), plan);
    assert.eq(executionStages.works, trialLengthFromCollFrac, plan);
}

// Verifies that for each SBE plan in the 'allPlans' array, the number of storage reads done by the
// plan is equal to 'expectedNumReads'.
function verifySbeNumReads(allPlans, expectedNumReads) {
    for (let plan of allPlans) {
        // Infer the number of reads (SBE's equivalent of work units) as the sum of keys and
        // documents examined.
        assert(plan.hasOwnProperty("totalKeysExamined"), plan);
        assert(plan.hasOwnProperty("totalDocsExamined"), plan);
        const numReads = plan.totalKeysExamined + plan.totalDocsExamined;
        assert.eq(numReads, expectedNumReads, plan);
    }
}

// Allow the query to use SBE. This time, the trial period should terminate based on the works knob.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));
allPlans = getAllPlansExecution("2");
verifySbeNumReads(allPlans, trialLengthFromWorksKnob);

// If the SBE "collection fraction" knob is set to the same value as the equivalent knob for the
// classic engine, then the SBE trial period should now terminate based on this new value of the
// collection fraction knob.
assert.commandWorked(db.adminCommand({setParameter: 1, [collFracKnobSbe]: defaultCollFrac}));
allPlans = getAllPlansExecution("2");
verifySbeNumReads(allPlans, trialLengthFromCollFrac);

MongoRunner.stopMongod(conn);
}());
