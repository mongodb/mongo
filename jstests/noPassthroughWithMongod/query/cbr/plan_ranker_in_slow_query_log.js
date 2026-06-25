/**
 * Tests that the slow query log and the profiler report which plan ranker ("planRanker") selected
 * the winning plan: "cbr" when the cost-based ranker chose it, and "mp" when the multi-planner did.
 *
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {setCBRConfig} from "jstests/libs/query/cbr_utils.js";

const collName = jsTestName();
const coll = db[collName];

describe("planRanker in slow query log and profiler", function () {
    before(function () {
        // Two competing single-field indexes plus a predicate on each field guarantees that more
        // than one candidate plan is enumerated, so plan ranking actually takes place.
        coll.drop();
        const docs = [];
        for (let i = 0; i < 1000; i++) {
            docs.push({a: i % 5, b: i});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
    });

    // Runs a find query and returns the 'planRanker' value captured from both the slow query log
    // line and the profiler entry (undefined if absent in that surface).
    function runAndGetPlanRanker(marker) {
        coll.getPlanCache().clear();

        const predicate = {a: 1, b: 6};

        // Log every operation (slowms: 0).
        try {
            assert.commandWorked(db.setProfilingLevel(2, {slowms: 0}));
            assert.eq(coll.find(predicate).comment(marker).itcount(), 1);
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

    it("reports 'cbr' when the cost-based ranker chooses the winning plan", function () {
        // TODO (SERVER-119581): Remove once featureFlagGetExecutorDeferredEngineChoice is removed.
        if (!FeatureFlagUtil.isEnabled(db, "GetExecutorDeferredEngineChoice")) {
            jsTest.log.info(
                "Skipping: featureFlagGetExecutorDeferredEngineChoice is disabled, " +
                    "CBR is not invoked without the deferred engine-choice path.",
            );
            return;
        }

        setCBRConfig(db, {internalQueryCBRCEMode: "samplingCE"});
        const {fromLog, fromProfiler} = runAndGetPlanRanker("planRankerMarkerCbr");
        assert.eq(fromLog, "cbr", "slow query log should report planRanker: cbr");
        assert.eq(fromProfiler, "cbr", "profiler should report planRanker: cbr");
    });

    it("reports 'mp' when the multi-planner chooses the winning plan", function () {
        // Disabling CBR forces the multi-planner to select the winning plan at runtime.
        setCBRConfig(db, {featureFlagCostBasedRanker: false});
        const {fromLog, fromProfiler} = runAndGetPlanRanker("planRankerMarkerMp");
        assert.eq(fromLog, "mp", "slow query log should report planRanker: mp");
        assert.eq(fromProfiler, "mp", "profiler should report planRanker: mp");
    });

    it("reports 'none' when a cached plan is used (no ranking took place)", function () {
        setCBRConfig(db, {featureFlagCostBasedRanker: false});

        const predicate = {a: 1, b: 6};
        coll.getPlanCache().clear();
        coll.find(predicate).itcount();
        coll.find(predicate).itcount(); // Second miss: promotes entry to active

        // Third run: no ranking takes place.
        const marker = "planRankerMarkerCacheHit";
        try {
            assert.commandWorked(db.setProfilingLevel(2, {slowms: 0}));
            assert.eq(coll.find(predicate).comment(marker).itcount(), 1);
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
            "none",
            "cache hit should report planRanker: none",
        );
        assert.eq(profileEntry.planRanker, "none", "cache hit should report planRanker: none");
    });

    it("reports 'none' when only one candidate plan exists (no ranking needed)", function () {
        setCBRConfig(db, {featureFlagCostBasedRanker: false});
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
            "none",
            "single-solution query should report planRanker: none",
        );
        assert.eq(
            profileEntry.planRanker,
            "none",
            "single-solution query should report planRanker: none",
        );
    });
});
