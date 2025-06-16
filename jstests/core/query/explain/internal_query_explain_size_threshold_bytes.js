/**
 * Tests that the SBE slots and SBE stages fields of explain respect
 * internalQueryExplainSizeThresholdBytes.
 * @tags: [
 *  # For simplicity of explain analysis, this test does not run against sharded collections.
 *  assumes_against_mongod_not_mongos,
 *  assumes_standalone_mongod,
 *  assumes_unsharded_collection,
 *  # Refusing to run a test that issues commands that may return different values after a failover
 *  does_not_support_stepdowns,
 *  # Explain for the aggregate command cannot run within a multi-document transaction
 *  does_not_support_transactions,
 *  requires_fcv_82]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {
    getEngine,
    getQueryPlanner,
    getSingleNodeExplain,
    getWarnings
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const leeway = 32;
const statsTreeWarning = "stats tree exceeded BSON size limit for explain";
const slotBasedPlanWarning = "slotBasedPlan exceeded BSON size limit for explain";
const capWarning = "exceeded explain BSON size cap";

function fitsPlanWarning(winningPlan) {
    jsTestLog("fitsPlanWarning");
    return winningPlan.hasOwnProperty("warning") && winningPlan.warning == slotBasedPlanWarning;
}

function fitsPlanSlotsStageWarning(winningPlan) {
    jsTestLog("fitsPlanSlotsStageWarning");
    return winningPlan.hasOwnProperty("slotBasedPlan") &&
        winningPlan.slotBasedPlan.hasOwnProperty("slots") &&
        !winningPlan.slotBasedPlan.hasOwnProperty("stages") &&
        winningPlan.slotBasedPlan.hasOwnProperty("warning") &&
        winningPlan.slotBasedPlan.warning == capWarning;
}

function fitsPlanSlotsStages(winningPlan) {
    jsTestLog("fitsPlanSlotsStages");
    return winningPlan.hasOwnProperty("slotBasedPlan") &&
        winningPlan.slotBasedPlan.hasOwnProperty("slots") &&
        winningPlan.slotBasedPlan.hasOwnProperty("stages") &&
        !winningPlan.slotBasedPlan.hasOwnProperty("warning");
}

const original = assert.commandWorked(
    db.adminCommand({getParameter: 1, "internalQueryExplainSizeThresholdBytes": 1}));

try {
    // Increase explain threshold step-by-step.
    for (let size = 350;; size++) {
        if (!checkSbeRestrictedOrFullyEnabled(db)) {
            // Test is SBE-only.
            break;
        }
        setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                               "internalQueryExplainSizeThresholdBytes",
                               size);
        const coll = db.internal_query_explain_size_threshold_bytes;
        coll.drop();
        assert.commandWorked(coll.insert({_id: 1, a: 1}));
        let orClauses = [];
        for (let i = 0; i < 2; i++) {
            orClauses.push({"a": i});
        }
        const explain = coll.explain().aggregate([
            {$match: {"$or": orClauses}},
            {$group: {_id: "$_id"}},
            {$project: {_id: 1, a: 0}},
        ]);
        // Test is SBE-only. Assert the query used SBE as expected.
        assert(getWarnings(explain).length > 0 || getEngine(explain) === "sbe");
        jsTestLog("Checking explain");
        let winningPlan = getQueryPlanner(explain).winningPlan;
        let queryPlan = winningPlan.queryPlan;
        let slotBasedPlan = winningPlan.slotBasedPlan;
        jsTestLog({
            "size": size,
            "Object.bsonsize(winningPlan)": Object.bsonsize(winningPlan),
            "winningPlan": winningPlan,
        });
        assert(fitsPlanWarning(winningPlan) || fitsPlanSlotsStageWarning(winningPlan) ||
               fitsPlanSlotsStages(winningPlan));
        // At very small thresholds, the error messages can cause the explain's BSON size to
        // exceed the threshold. Give ourselves 32 bytes of leeway for the comparison here.
        assert(Object.bsonsize(winningPlan) <= size + leeway);
        if (fitsPlanSlotsStages(winningPlan)) {
            // Everything fits.
            break;
        }
    }
} finally {
    // Reset parameter for other tests.
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()),
                           "internalQueryExplainSizeThresholdBytes",
                           original.internalQueryExplainSizeThresholdBytes);
}
