/**
 * Regression test for SERVER-135566: the index seek estimate must take into account leading point
 * intervals, which come before the first range. Although a point interval does not multiply the
 * number of seeks, it restricts which keys the index scan can reach. A point interval after a range
 * does not reduce the seeks, since the scan still seeks for every distinct value of the range.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAllPlans, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagCostBasedRanker: true,
        internalQueryCBRCEMode: "samplingCE",
        internalQueryPlanRanker: "costBased",
        internalQuerySamplingBySequentialScan: true,
    },
});
const db = conn.getDB("test");

// TODO SERVER-130973: Remove this skip once CBR supports SBE.
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info("Skipping test: CBR does not support SBE");
    MongoRunner.stopMongod(conn);
    quit();
}

describe("index seek estimate with a point prefix", function () {
    const coll = db[jsTestName()];
    const numDocs = 1000;
    const numDistinctA = 10;

    before(function () {
        coll.drop();
        // 'a' takes values 1..numDistinctA (never 0), 'b' is unique, 'c' is always 0.
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({a: (i % numDistinctA) + 1, b: i + 1, c: 0});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndex({a: 1, b: 1, c: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    function getSeekEstimate(query) {
        const explain = assert.commandWorked(coll.find(query).explain());
        const plans = getAllPlans(explain);
        assert.eq(plans.length, 1, "expected exactly one plan", {explain});
        assertPlanCosted(plans[0]);

        const winningPlan = getWinningPlanFromExplain(explain);
        const ixscan = winningPlan.inputStage ?? winningPlan;
        assert.eq(ixscan.stage, "IXSCAN", "winning plan should be an index scan", {explain});
        assert(ixscan.hasOwnProperty("indexSeekEstimate"), "missing indexSeekEstimate", {explain});
        return ixscan.indexSeekEstimate;
    }

    it("point prefix matching no documents yields a single seek", function () {
        // No document has a: 0, so the scan seeks once and finds nothing. Without the point interval
        // on 'a', NDV(b) over b > 0 would estimate one seek per document.
        assert.eq(getSeekEstimate({a: 0, b: {$gt: 0}, c: 0}), 1);
    });

    it("point prefix restricts the NDV of the following range", function () {
        // Only numDocs / numDistinctA documents have a: 5, so the seek estimate must be well below
        // the collection size.
        const estimate = getSeekEstimate({a: 5, b: {$gt: 0}, c: 0});
        assert.gt(estimate, 1, "expected more than one seek", {estimate});
        assert.lt(estimate, numDocs / 2, "point prefix on 'a' should restrict the estimate", {
            estimate,
        });
    });

    it("point after a range does not restrict the NDV of the range", function () {
        // The scan seeks once per distinct 'a', even though no document has b: 0. The estimate is
        // NDV(a) = numDistinctA; the point interval on 'b' must not reduce it to a single seek.
        assert.eq(getSeekEstimate({a: {$gt: 0}, b: 0, c: 0}), numDistinctA);
    });
});
