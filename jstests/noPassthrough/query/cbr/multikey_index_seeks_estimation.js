/**
 * Regression test for SERVER-129532: samplingCE must not crash when estimating cardinality for a
 * query whose predicates span fields that participate in a compound multikey index containing a
 * dotted path that traverses an array.
 */
import {after, describe, it} from "jstests/libs/mochalite.js";
import {
    assertChosenRanker,
    ChosenRanker,
    getAllPlans,
    getPlanStage,
    getRejectedPlans,
    getWinningPlanFromExplain,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, ceEqual} from "jstests/libs/query/cbr_utils.js";
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
    jsTestLog("Skipping test: CBR does not support SBE");
    MongoRunner.stopMongod(conn);
    quit();
}

describe("samplingCE with dotted paths sharing an array-valued prefix", function () {
    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Recreates the collection with 'indexes' and a single 'doc', then explains 'query' to assert
    // that the winning plan is an index scan over the expected index and that the seek estimate
    // matches the expected number of seeks.
    function assertEstimateMatchesActual({index, doc, query, expectedIndexName, expectedSeeks}) {
        const coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(coll.createIndex(index));
        assert.commandWorked(coll.insert(doc));

        const explain = assert.commandWorked(coll.find(query).explain());

        const plans = getAllPlans(explain);
        assert.eq(plans.length, 1, "expected exactly one plan", {explain});
        assertPlanCosted(plans[0]);

        const winningPlan = getWinningPlanFromExplain(explain);
        const ixscan = winningPlan.inputStage ?? winningPlan;
        assert.eq(ixscan.stage, "IXSCAN", "winning plan should be an index scan", {explain});
        assert.eq(ixscan.indexName, expectedIndexName, "winning plan used the wrong index", {
            explain,
        });
        assert(ixscan.isMultiKey, "winning plan's index should be multikey", {explain});

        // The seek estimate is produced by the multikey array-unwinding NDV path under test.
        assert.eq(
            ixscan.indexSeekEstimate,
            expectedSeeks,
            "index scan seek estimate should match the expected multikey key count",
            {indexSeekEstimate: ixscan.indexSeekEstimate, expectedSeeks, explain},
        );
    }

    it("multikey compound index with leading $in", function () {
        // Index {"d.a": 1, "e": 1}: the leading $in on "d.a" yields multi-point bounds that survive
        // seek estimation, so CE unwinds the "d" array for the single field "d.a".
        assertEstimateMatchesActual({
            index: {"d.a": 1, "e": 1},
            doc: {d: [{a: 0}, {a: 1}], e: 0},
            query: {"d.a": {$in: [0, 1]}, e: {$eq: 0}},
            expectedIndexName: "d.a_1_e_1",
            // estimateNDV([{d.a}], ...) produces 2 keys: (d.a: 0) and (d.a: 1). The trailing
            // predicate on 'e' adds a multiplication factor of 1, since it is a single equality.
            expectedSeeks: 2,
        });
    });

    it("field and dotted subfield sharing an array prefix", function () {
        // "d" is an array mixing a subdocument and a trailing scalar, indexed by {d, "d.a", e}. The
        // $in on "d" and "d.a" keeps both as a surviving bounds prefix, so CE unwinds "d" once and
        // resolves both "d" and "d.a" against each element -- the scalar element yields null for
        // "d.a".
        assertEstimateMatchesActual({
            index: {d: 1, "d.a": 1, "e": 1},
            doc: {d: [{a: 0}, 5], e: 0},
            query: {d: {$in: [{a: 0}, 5]}, "d.a": {$lt: 1}, e: {$in: [1, 2]}},
            expectedIndexName: "d_1_d.a_1_e_1",
            // estimateNDV([{d, d.a}], ...) produces 2 keys: ('d': {a: 0}, 'd.a': 0) and ('d': 5,
            // 'd.a': null). The trailing predicate on 'e' adds a multiplication factor of 2, one
            // for each Interval.
            expectedSeeks: 4,
        });
    });

    it("two dotted subfields sharing an array prefix (alternate-missing)", function () {
        // Each "d" element carries either an "a" or a "b" subfield, so "d.a" and "d.b" are present
        // on different elements of the same array (each null where the other is present). Both are
        // indexed by {"d.a", "d.b", e} and kept as a surviving $in prefix, exercising correlated
        // unwinding of two dotted subfields over one array.
        assertEstimateMatchesActual({
            index: {"d.a": 1, "d.b": 1, "e": 1},
            doc: {d: [{a: 0}, {b: 1}], e: 0},
            query: {"d.a": {$in: [0, 1]}, "d.b": {$in: [0, 1]}, e: {$in: [1, 2]}},
            expectedIndexName: "d.a_1_d.b_1_e_1",
            // estimateNDV([{d.a, d.b}], ...) produces 2 keys: (d.a: 0, d.b: null) and (d.a: null,
            // d.b: 1). The trailing predicate on 'e' adds a multiplication factor of 2, one for
            // each Interval.
            expectedSeeks: 4,
        });
    });

    it("dotted subfield that is itself an array (uneven nested arrays)", function () {
        // "d.a" is itself an array, so unwinding "d" reaches an element whose "a" subpath is another
        // array that must be unwound in turn (uneven nested arrays).
        assertEstimateMatchesActual({
            index: {d: 1, "d.a": 1, "e": 1},
            doc: {d: [5, {a: [0, 1]}], e: 0},
            query: {d: {$in: [{a: [0, 1]}, 5]}, "d.a": {$in: [0, 1, 2]}, e: {$eq: 0}},
            expectedIndexName: "d_1_d.a_1_e_1",
            // estimateNDV([{d, d.a}], ...) produces 3 keys: (d: 5, d.a: null), (d: {a: [0, 1]},
            // d.a: 0), and (d: {a: [0, 1]}, d.a: 1). The trailing predicate on 'e' adds a
            // multiplication factor of 1, since it is a single equality.
            expectedSeeks: 3,
        });
    });
});
