/**
 * Tests that the slow query log and the profiler report which plan ranker ("planRanker") selected
 * the winning plan: "costBased" when the cost-based ranker chose it, and "multiPlanning" when the
 * multi-planner did.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_profiling,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";

const collName = jsTestName();
const coll = db[collName];

const kNumDocs = 1000;
// The predicate used throughout. Both fields are low-cardinality (see 'before') so that every
// conjunct matches a large fraction of the collection: under samplingCE, a predicate matching only
// a handful of documents may be absent from the sample entirely, and a resulting zero cardinality
// estimate aborts CBR in favor of the multi-planner - which would make the "cbr" expectation below
// flaky.
const kPredicate = {a: 1, b: 6};
// 'a' is i%5 and 'b' is i%10, so b==6 implies a==1: every tenth document matches both conjuncts.
const kMatchingDocs = kNumDocs / 10;

describe("planRanker in slow query log and profiler", function () {
    before(function () {
        // Two competing single-field indexes plus a predicate on each field guarantees that more
        // than one candidate plan is enumerated, so plan ranking actually takes place.
        coll.drop();
        const docs = [];
        for (let i = 0; i < kNumDocs; i++) {
            docs.push({a: i % 5, b: i % 10});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

        // Guard the assumption above: each conjunct, and the conjunction, must match enough
        // documents that any sample contains some of them.
        assert.eq(coll.find(kPredicate).itcount(), kMatchingDocs);
        assert.gte(coll.find({a: kPredicate.a}).itcount(), kMatchingDocs);
        assert.gte(coll.find({b: kPredicate.b}).itcount(), kMatchingDocs);
    });

    // Runs a find query and returns the 'planRanker' value captured from both the slow query log
    // line and the profiler entry (undefined if absent in that surface).
    function runAndGetPlanRanker(marker) {
        coll.getPlanCache().clear();

        // Log every operation (slowms: 0).
        try {
            assert.commandWorked(db.setProfilingLevel(2, {slowms: 0}));
            assert.eq(coll.find(kPredicate).comment(marker).itcount(), kMatchingDocs);
        } finally {
            assert.commandWorked(db.setProfilingLevel(0));
        }

        const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
        // Filter by ns to avoid matching internal sampling sub-queries (e.g. system.stats.samples).
        const logLine = findMatchingLogLine(log, {
            msg: "Slow query",
            comment: marker,
            ns: `test.${coll.getName()}`,
        });
        assert(logLine, "did not find a 'Slow query' log line for the query");
        const profileEntry = getLatestProfilerEntry(db, {op: "query", "command.comment": marker});

        return {
            fromLog: JSON.parse(logLine).attr.planRanker,
            fromProfiler: profileEntry.planRanker,
        };
    }

    it("reports 'costBased' when the cost-based ranker chooses the winning plan", function () {
        // TODO (SERVER-119581): Remove once featureFlagGetExecutorDeferredEngineChoice is removed.
        if (!FeatureFlagUtil.isEnabled(db, "GetExecutorDeferredEngineChoice")) {
            jsTest.log.info(
                "Skipping: featureFlagGetExecutorDeferredEngineChoice is disabled, " +
                    "CBR is not invoked without the deferred engine-choice path.",
            );
            return;
        }

        setPlanRankerConfig(db, {
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
        });
        const {fromLog, fromProfiler} = runAndGetPlanRanker("planRankerMarkerCbr");
        assert.eq(fromLog, "costBased", "slow query log should report planRanker: costBased");
        assert.eq(fromProfiler, "costBased", "profiler should report planRanker: costBased");
    });

    it("reports 'multiPlanning' when the multi-planner chooses the winning plan", function () {
        // Disabling CBR forces the multi-planner to select the winning plan at runtime.
        setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
        const {fromLog, fromProfiler} = runAndGetPlanRanker("planRankerMarkerMp");
        assert.eq(
            fromLog,
            "multiPlanning",
            "slow query log should report planRanker: multiPlanning",
        );
        assert.eq(
            fromProfiler,
            "multiPlanning",
            "profiler should report planRanker: multiPlanning",
        );
    });

    it("reports 'cachedPlan' when a cached plan is used (no ranking took place)", function () {
        setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});

        coll.getPlanCache().clear();
        coll.find(kPredicate).itcount();
        coll.find(kPredicate).itcount(); // Second miss: promotes entry to active

        // Third run: no ranking takes place.
        const marker = "planRankerMarkerCacheHit";
        try {
            assert.commandWorked(db.setProfilingLevel(2, {slowms: 0}));
            assert.eq(coll.find(kPredicate).comment(marker).itcount(), kMatchingDocs);
        } finally {
            assert.commandWorked(db.setProfilingLevel(0));
        }

        const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
        const logLine = findMatchingLogLine(log, {
            msg: "Slow query",
            comment: marker,
            ns: `test.${coll.getName()}`,
        });
        assert(logLine, "did not find slow query log line for cache-hit query");
        const profileEntry = getLatestProfilerEntry(db, {op: "query", "command.comment": marker});

        assert.eq(
            JSON.parse(logLine).attr.planRanker,
            "cachedPlan",
            "cache hit should report planRanker: cachedPlan",
        );
        assert.eq(
            profileEntry.planRanker,
            "cachedPlan",
            "cache hit should report planRanker: cachedPlan",
        );
    });

    it("reports 'express' for the express path, which bypasses plan selection", function () {
        setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});

        // The express path runs no plan selection, so it reports its own strategy rather than
        // claiming a single candidate plan existed.
        const id = coll.findOne()._id;
        const marker = "planRankerMarkerExpress";
        try {
            assert.commandWorked(db.setProfilingLevel(2, {slowms: 0}));
            assert.eq(coll.find({_id: id}).comment(marker).itcount(), 1);
        } finally {
            assert.commandWorked(db.setProfilingLevel(0));
        }

        const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
        const logLine = findMatchingLogLine(log, {
            msg: "Slow query",
            comment: marker,
            ns: `test.${coll.getName()}`,
        });
        assert(logLine, "did not find slow query log line for express query");
        const profileEntry = getLatestProfilerEntry(db, {op: "query", "command.comment": marker});

        assert.eq(
            JSON.parse(logLine).attr.planRanker,
            "express",
            "express query should report planRanker: express",
        );
        assert.eq(
            profileEntry.planRanker,
            "express",
            "express query should report planRanker: express",
        );
    });

    it("reports 'singlePlan' when only one candidate plan exists (no ranking needed)", function () {
        setPlanRankerConfig(db, {featureFlagCostBasedRanker: false});
        coll.getPlanCache().clear();

        // Field 'c' has no index, so the planner produces only a COLLSCAN — a single candidate.
        const marker = "planRankerMarkerSingleSolution";
        try {
            assert.commandWorked(db.setProfilingLevel(2, {slowms: 0}));
            assert.gte(coll.find({c: 42}).comment(marker).itcount(), 0);
        } finally {
            assert.commandWorked(db.setProfilingLevel(0));
        }

        const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
        const logLine = findMatchingLogLine(log, {
            msg: "Slow query",
            comment: marker,
            ns: `test.${coll.getName()}`,
        });
        assert(logLine, "did not find slow query log line for single-solution query");
        const profileEntry = getLatestProfilerEntry(db, {op: "query", "command.comment": marker});

        assert.eq(
            JSON.parse(logLine).attr.planRanker,
            "singlePlan",
            "single-solution query should report planRanker: singlePlan",
        );
        assert.eq(
            profileEntry.planRanker,
            "singlePlan",
            "single-solution query should report planRanker: singlePlan",
        );
    });
});
