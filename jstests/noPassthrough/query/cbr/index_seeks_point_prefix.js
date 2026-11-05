/**
 * Regression test for SERVER-135566: the index seek estimate must take into account leading point
 * intervals, which come before the first range. Although a point interval does not multiply the
 * number of seeks, it restricts which keys the index scan can reach. A point interval after a range
 * does not reduce the seeks, since the scan still seeks for every distinct value of the range.
 * Uses V3 explain to expose index seek estimates under both the classic engine and SBE.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getAllPlans,
    getPlanStage,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted} from "jstests/libs/query/cbr_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagCostBasedRanker: true,
        internalQueryCBRCEMode: "samplingCE",
        internalQueryPlanRanker: "costBased",
        internalQuerySamplingBySequentialScan: true,
    },
});
const db = conn.getDB("test");

describe("index seek estimate with a point prefix", function () {
    const coll = db[jsTestName()];
    const numDocs = 100;
    const numDistinctA = 10;

    before(function () {
        coll.drop();
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({a: (i % numDistinctA) + 1, b: i, c: 0});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Returns the sole candidate plan for 'explain'.
    function getSinglePlan(explain) {
        const plans = getAllPlans(explain);
        assert.eq(plans.length, 1, "expected exactly one plan", {explain});
        return plans[0];
    }

    function getSeekEstimate(explain) {
        const winningPlan = getWinningPlanFromExplain(explain);
        const ixscan = getPlanStage(winningPlan, "IXSCAN");
        assert.neq(ixscan, null, "winning plan should be an index scan", {explain});
        const estimates = ixscan.statistics.costBased;
        assert(estimates.hasOwnProperty("indexSeekEstimate"), "missing indexSeekEstimate", {
            explain,
        });
        return estimates.indexSeekEstimate;
    }

    it("point prefix matching no documents yields a single seek", function () {
        // No document has a: 0, so the scan would seek once and find nothing.
        // Without the point interval on 'a', NDV(b) over b > 0 would estimate
        // one seek per document.
        const explain = assert.commandWorked(
            coll.find({a: 0, b: {$gt: 0}, c: 0}).explain("plannerStats"),
        );
        assertPlanNotCosted(getSinglePlan(explain));
        assert.eq(getSeekEstimate(explain), 1);
    });

    it("point prefix restricts the NDV of the following range", function () {
        // Only numDocs / numDistinctA documents have a: 5, so the seek estimate must be well below
        // the collection size.
        const explain = assert.commandWorked(
            coll.find({a: 5, b: {$gt: 0}, c: 0}).explain("plannerStats"),
        );
        assertPlanCosted(getSinglePlan(explain));
        const estimate = getSeekEstimate(explain);
        assert.gt(estimate, 1, "expected more than one seek", {estimate});
        assert.lt(estimate, numDocs / 2, "point prefix on 'a' should restrict the estimate", {
            estimate,
        });
    });

    it("point after a range does not restrict the NDV of the range", function () {
        // The scan seeks once per distinct 'a' above zero, so the estimate is NDV(a) =
        // numDistinctA; the point interval on 'b' must not reduce it to a single seek.
        const explain = assert.commandWorked(
            coll.find({a: {$gt: 0}, b: 0, c: 0}).explain("plannerStats"),
        );
        assertPlanCosted(getSinglePlan(explain));
        assert.eq(Math.floor(getSeekEstimate(explain)), numDistinctA);
    });
});
