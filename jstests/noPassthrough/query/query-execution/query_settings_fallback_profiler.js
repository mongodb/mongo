/**
 * Tests that the failedPlanningWithQuerySettings flag is correctly set in the profiler when
 * query settings fail to produce a valid plan and the planner falls back to multi-planning.
 * @tags: [
 *   requires_profiling,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("failedPlanningWithQuerySettings profiler flag", function () {
    let rst;
    let testDB;
    let coll;
    let qsutils;
    const profileEntryFilter = {op: "query", "command.find": "test"};

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet({setParameter: {logComponentVerbosity: tojson({query: {verbosity: 2}})}});
        rst.initiate();

        testDB = rst.getPrimary().getDB(jsTestName());
        coll = testDB.getCollection("test");

        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
        for (let i = 0; i < 5; ++i) {
            assert.commandWorked(coll.insert({a: i, b: i}));
        }

        qsutils = new QuerySettingsUtils(testDB, coll.getName());
    });

    after(function () {
        rst.stopSet();
    });

    /**
     * Helper to clear the plan cache and profiler, ensuring a clean state for each test.
     */
    function resetProfiler() {
        coll.getPlanCache().clear();
        assert.commandWorked(testDB.setProfilingLevel(0));
        testDB.system.profile.drop();
        assert.commandWorked(testDB.setProfilingLevel(2));
    }

    it("should be true when query settings fallback is engaged", function () {
        const ns = {db: testDB.getName(), coll: coll.getName()};
        const querySettingsQuery = qsutils.makeFindQueryInstance({filter: {a: 1, b: 1}});
        const settings = {indexHints: {ns, allowedIndexes: ["doesnotexist"]}};

        qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            resetProfiler();
            // Use find().toArray() instead of findOne() to avoid the express fast path
            // which bypasses the planner and would not trigger the fallback.
            const results = coll.find({a: 1, b: 1}).toArray();
            assert.gt(results.length, 0);
            const profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
            assert.eq(profileObj.failedPlanningWithQuerySettings, true, profileObj);
        });
    });

    it("should be absent when no query settings are present", function () {
        resetProfiler();
        const results = coll.find({a: 1, b: 1}).toArray();
        assert.gt(results.length, 0);
        const profileObj = getLatestProfilerEntry(testDB, profileEntryFilter);
        assert(!profileObj.hasOwnProperty("failedPlanningWithQuerySettings"), profileObj);
    });
});
