/**
 * Tests the behavior of the internalQueryPlanTotalEvaluationCollFraction parameter.
 */
import {
    assertChosenRanker,
    ChosenRanker,
    getEngine,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {getPlanRanker} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/query/sbe_util.js";
import {describe, it, beforeEach, after} from "jstests/libs/mochalite.js";

const dbName = jsTestName();
const collName = jsTestName();

const conn = MongoRunner.runMongod({});
const db = conn.getDB(dbName);
const coll = db.getCollection(collName);
const sbeFeatureFlagFullEnabled = checkSbeFullFeatureFlagEnabled(db);

function resetKnobsForTest() {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: true}));
    // To avoid needing a huge collection to see the effects of the collFraction limit kick in.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 1}));
    // Ensure total coll fraction is always used in this test instead of the per-plan limit.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationCollFraction: 1}),
    );
}

function generateCombinations(n) {
    let combinations = [[]];
    for (let i = 0; i < n; i++) {
        const newCombinations = [];
        for (const combo of combinations) {
            newCombinations.push([...combo, 1]);
            newCombinations.push([...combo, -1]);
        }
        combinations = newCombinations;
    }
    return combinations;
}

function generateIndexes(n) {
    const nonSelectiveFields = [
        "nonSelective1",
        "nonSelective2",
        "nonSelective3",
        "nonSelective4",
        "nonSelective5",
        "nonSelective6",
    ];
    const selectiveField = "selective";

    const directionCombinations = generateCombinations(nonSelectiveFields.length).slice(0, n);
    return directionCombinations.map((directions, index) => {
        const indexSpec = {};

        nonSelectiveFields.forEach((field, fieldIndex) => {
            indexSpec[field] = directions[fieldIndex];
        });
        indexSpec[selectiveField] = 1;
        print(`Created index ${index + 1}: ${tojson(indexSpec)}`);
        return indexSpec;
    });
}

function setUpCollection() {
    coll.drop();

    const numDocs = 10;
    const bulkOp = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulkOp.insert({
            _id: i,
            nonSelective1: i,
            nonSelective2: i,
            nonSelective3: i,
            nonSelective4: i,
            nonSelective5: i,
            nonSelective6: i,
        });
    }
    assert.commandWorked(bulkOp.execute());
    assert(coll.count() == numDocs);

    // Create enough indexes to exercise the collFraction limit.
    const indexes = generateIndexes(15);
    assert.commandWorked(coll.createIndexes(indexes));
}

const query = {
    nonSelective1: {$gte: 0},
    nonSelective2: {$gte: 0},
    nonSelective3: {$gte: 0},
    nonSelective4: {$gte: 0},
    nonSelective5: {$gte: 0},
    nonSelective6: {$gte: 0},
    selective: {$lt: 0},
};

// Reports which multi-planner stopping condition metric increased after running a query
// with the given 'collFraction' setting.
function getStoppingCondition(collFraction, limit = 0) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryPlanTotalEvaluationCollFraction: collFraction,
        }),
    );

    const before = db.serverStatus().metrics.query.multiPlanner.stoppingCondition;
    // Note: limit = 0 is treated as no limit.
    coll.find(query).limit(limit).explain();
    const after = db.serverStatus().metrics.query.multiPlanner.stoppingCondition;
    return {
        hitEof: after.hitEof - before.hitEof,
        hitResultsLimit: after.hitResultsLimit - before.hitResultsLimit,
        hitWorksLimit: after.hitWorksLimit - before.hitWorksLimit,
    };
}

const planRanker = getPlanRanker(db);
describe("MultiPlanner exit condition metrics get updated correctly", function () {
    beforeEach(function () {
        setUpCollection();
        resetKnobsForTest();
    });

    if (planRanker === "costBased") {
        throw new Error(`This test does not support planRanker=costBased`);
    } else if (planRanker === "mixed" && !sbeFeatureFlagFullEnabled) {
        describe("Mixed plan ranker", function () {
            describe("fallback to CBR", function () {
                // The query matches nothing on a fully-sampled collection, so CBR's scan
                // estimates are zero (ZeroCEEstimate) and CBR hands ranking back to the
                // multi-planner, whose stopping condition is recorded regardless of the plan
                // cache setting.
                it("records the multi-planner stopping condition when plan cache is disabled", function () {
                    assert.commandWorked(
                        db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: true}),
                    );

                    assert.docEq(
                        {
                            hitEof: 0,
                            hitResultsLimit: 0,
                            hitWorksLimit: 1,
                        },
                        getStoppingCondition(0.1),
                    );

                    assert.docEq(
                        {
                            hitEof: 1,
                            hitResultsLimit: 0,
                            hitWorksLimit: 0,
                        },
                        getStoppingCondition(20.0),
                    );
                });

                it("does not update multi-planner metrics when CBR chooses the winner", function () {
                    // One matching document exists, but it sits beyond the trial phase's works
                    // budget, so multi-planning produces no result and hands over to CBR. The
                    // sample does observe the document, so CBR costs the plans and picks the
                    // winner. We do not measure works for the chosen CBR plan, so with the plan
                    // cache disabled the MP metrics must not change.
                    coll.insert({
                        nonSelective1: 0,
                        nonSelective2: 0,
                        nonSelective3: 0,
                        nonSelective4: 0,
                        nonSelective5: 0,
                        nonSelective6: 0,
                        selective: -1,
                    });

                    assert.commandWorked(
                        db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: true}),
                    );

                    assert.docEq(
                        {
                            hitEof: 0,
                            hitResultsLimit: 0,
                            hitWorksLimit: 0,
                        },
                        getStoppingCondition(0.1),
                    );

                    const explain = coll.find(query).explain();
                    // TODO SERVER-117707: CBR is not engaged for SBE-targeted queries, so the
                    // winner is only costed under the classic engine. Drop the guard once CBR
                    // supports SBE.
                    if (getEngine(explain) === "classic") {
                        assertChosenRanker(explain, {
                            chosenRanker: ChosenRanker.kCostBased,
                            reason: PlanRankerReason.kNoMultiplanningResults,
                        });
                    }
                });

                it("records hitWorksLimit with low collFraction", function () {
                    // At low collFraction, the resumed multi-planner stops on its works budget.
                    assert.commandWorked(
                        db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: false}),
                    );

                    assert.docEq(
                        {
                            hitEof: 0,
                            hitResultsLimit: 0,
                            hitWorksLimit: 1,
                        },
                        getStoppingCondition(0.1),
                    );
                });

                it("records hitEof with high collFraction", function () {
                    // With a higher collFraction, the resumed multi-planner runs until EOF.
                    assert.commandWorked(
                        db.adminCommand({setParameter: 1, internalQueryDisablePlanCache: false}),
                    );

                    assert.docEq(
                        {
                            hitEof: 1,
                            hitResultsLimit: 0,
                            hitWorksLimit: 0,
                        },
                        getStoppingCondition(20.0),
                    );
                });
            });

            describe("no fallback to CBR", function () {
                it("records EOF when multiplanning produces a result before exhausting works", function () {
                    // Insert 1 matching document so multiplanning returns a result and does not fallback to CBR.
                    coll.insert({
                        nonSelective1: 0,
                        nonSelective2: 0,
                        nonSelective3: 0,
                        nonSelective4: 0,
                        nonSelective5: 0,
                        nonSelective6: 0,
                        selective: -1,
                    });

                    // Ensure the first-phase MP trials have enough works to reach the matching doc and EOF.
                    assert.commandWorked(
                        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 15}),
                    );

                    assert.docEq(
                        {
                            hitEof: 1,
                            hitResultsLimit: 0,
                            hitWorksLimit: 0,
                        },
                        getStoppingCondition(0.1),
                    );
                });

                it("records hitResultsLimit when multiplanning hits the batch limit", function () {
                    coll.insert({
                        nonSelective1: 0,
                        nonSelective2: 0,
                        nonSelective3: 0,
                        nonSelective4: 0,
                        nonSelective5: 0,
                        nonSelective6: 0,
                        selective: -1,
                    });

                    assert.commandWorked(
                        db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 15}),
                    );

                    // Limit(1) forces the batch results limit to be the stopping condition.
                    assert.docEq(
                        {
                            hitEof: 0,
                            hitResultsLimit: 1,
                            hitWorksLimit: 0,
                        },
                        getStoppingCondition(0.1, 1),
                    );
                });
            });
        });
    } else {
        // planRanker == "multiPlanning" or SBE is enabled
        describe("multiPlanner (no CBR)", function () {
            it("records hitWorksLimit for low collFraction", function () {
                // With a low total collection fraction, multiplanning should stop due to works limit.
                assert.docEq(
                    {
                        hitEof: 0,
                        hitResultsLimit: 0,
                        hitWorksLimit: 1,
                    },
                    getStoppingCondition(0.1),
                );
            });

            it("records hitEof for high collFraction", function () {
                // With a higher collFraction, multiplanning continues until the first plan
                // scans the whole index and reaches EOF.
                assert.docEq(
                    {
                        hitEof: 1,
                        hitResultsLimit: 0,
                        hitWorksLimit: 0,
                    },
                    getStoppingCondition(20.0),
                );
            });
        });
    }
});
after(function () {
    MongoRunner.stopMongod(conn);
});
