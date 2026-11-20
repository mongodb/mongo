/**
 * @tags: [requires_fcv_91, requires_profiling, requires_sbe]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {getPlanStage, getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

describe("streaming $group plan cache invalidation", function () {
    let conn, testDb, coll;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagSbeStreamingGroup: true,
                featureFlagPathArrayness: true,
                internalEnablePathArrayness: true,
                internalQueryFrameworkControl: "trySbeRestricted",
                internalQueryPlannerUseMultiplannerForSingleSolutions: true,
            },
        });
        assert.neq(null, conn, "mongod was unable to start up");
        testDb = conn.getDB(jsTestName());
        coll = testDb.coll;
        assert.commandWorked(testDb.setProfilingLevel(2));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("evicts the active streaming plan when the grouping path becomes multikey", function () {
        assert.commandWorked(coll.createIndex({"a.b": 1}));
        assert.commandWorked(coll.insertMany([{a: {b: 1}}, {a: {b: 1}}, {a: {b: 2}}]));

        const pipeline = [{$match: {"a.b": {$gte: 1}}}, {$group: {_id: "$a.b", count: {$sum: 1}}}];
        const options = {allowDiskUse: true};
        const scalarGroups = [
            {_id: 1, count: 2},
            {_id: 2, count: 1},
        ];

        function runAggregate(comment, expected) {
            assert.sameMembers(expected, coll.aggregate(pipeline, {...options, comment}).toArray());
            return getLatestProfilerEntry(testDb, {
                "command.aggregate": coll.getName(),
                "command.comment": comment,
            });
        }

        function cacheEntries() {
            return coll.aggregate([{$planCacheStats: {}}]).toArray();
        }

        function assertGroupStage(entry, stage) {
            const planner = getQueryPlanner(coll.explain().aggregate(pipeline, options));
            assert.eq(entry.planCacheKey, planner.planCacheKey, {entry, planner});
            const stages = planner.winningPlan.slotBasedPlan.stages;
            assert(new RegExp(`\\b${stage}\\s*\\[`).test(stages), {stage, stages});
        }

        runAggregate("populate", scalarGroups);
        runAggregate("activate", scalarGroups);
        const entries = cacheEntries();
        assert.eq(1, entries.length, {entries});
        const [entry] = entries;
        assert.eq(true, entry.isActive, {entry});
        assert.eq(false, getPlanStage(entry.cachedPlan, "IXSCAN").isMultiKey, {entry});
        assertGroupStage(entry, "streaming_group");

        const hit = runAggregate("before multikey insert", scalarGroups);
        assert.eq(true, hit.fromPlanCache, {hit});
        assert.eq(entry.planCacheKey, hit.planCacheKey, {entry, hit});

        assert.commandWorked(coll.insertOne({a: [{b: 1}, {b: 2}]}));
        assert.eq([], cacheEntries(), "the multikey insert must evict the streaming plan");

        const miss = runAggregate("after multikey insert", [
            ...scalarGroups,
            {_id: [1, 2], count: 1},
        ]);
        assert(!miss.fromPlanCache, "the first execution after the insert must replan", {miss});

        const replacementEntries = cacheEntries();
        assert.eq(1, replacementEntries.length, {replacementEntries});
        const [replacement] = replacementEntries;
        assert.eq(miss.planCacheKey, replacement.planCacheKey, {miss, replacement});
        assert.eq(true, getPlanStage(replacement.cachedPlan, "IXSCAN").isMultiKey, {replacement});
        assertGroupStage(replacement, "group");
    });
});
