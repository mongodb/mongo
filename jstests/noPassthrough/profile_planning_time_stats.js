/**
 * Tests for validating that planning time stats are included in slow query logs.
 * @tags: [
 *   # Slow Windows machines cause this test to be flaky. Increasing maxPlanningTimeMicros would
 *   # make the test less useful on Linux variants so we don't run on Windows.
 *   incompatible_with_windows_tls,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    runWithFailpoint,
    setupCollectionAndGetExplainTestCases
} from "jstests/noPassthrough/explain_and_profile_optimization_stats_util.js";

const dbName = jsTestName();
const collName = "jstests_profile_stats";
const ns = dbName + "." + collName;
const viewNs = dbName + ".view";

/**
 * Increase the profiling level, turn off plan caching, and set up the failpoint specified in
 * 'testCase'. Then, run the command provided in 'testCase' against 'db'. Finally, extract the
 * profile entries matching the test case, asserting that we see the expected number of entries and
 * that the profile entries have 'planningTimeMicros' between 'minPlanningTime' and
 * 'maxPlanningTime'. Clears the profile collection between test runs.
 */
function testProfileEntryContainsPlanningTime(
    db, testCase, numExpectedEntries, minPlanningTime, maxPlanningTime) {
    const primaryDb = FixtureHelpers.getPrimaryForNodeHostingDatabase(db).getDB(db.getName());

    const comment = "profile_planning_stats";
    testCase.command.comment = comment;
    jsTestLog(`Testing command ${testCase.testName}: ${tojson(testCase.command)}`);

    assert.commandWorked(primaryDb.setProfilingLevel(0));
    primaryDb.system.profile.drop();
    assert.commandWorked(primaryDb.setProfilingLevel(2));
    assert.commandWorked(
        primaryDb.adminCommand({setParameter: 1, internalQueryDisablePlanCache: true}));
    assert.commandWorked(primaryDb.adminCommand(
        {setParameter: 1, logComponentVerbosity: {command: 5, replication: 5, query: 5}}));

    runWithFailpoint(db, testCase.failpointName, testCase.failpointOpts, () => {
        if (testCase.command.bulkWrite) {
            assert.commandWorked(db.adminCommand(testCase.command));
        } else {
            assert.commandWorked(db.runCommand(testCase.command));
        }

        const profileFilter = {"command.comment": comment, ns: {$in: [ns, viewNs]}};
        const profileEntries = primaryDb.system.profile.find(profileFilter).toArray();
        assert.eq(
            profileEntries.length,
            numExpectedEntries,
            "Did not find expected numer of profile entries. Found: " + tojson(profileEntries) +
                " in profile: " + tojson(primaryDb.system.profile.find().toArray()));
        profileEntries.forEach(
            entry => assert.betweenIn(
                minPlanningTime, entry.planningTimeMicros, maxPlanningTime, tojson(entry)));
    });
}

function runTest(db) {
    const waitTimeMillis = 500;
    const explainTestCases = setupCollectionAndGetExplainTestCases(db, collName, waitTimeMillis);

    function unwrapExplain(cmd) {
        if (typeof cmd.command.explain !== 'boolean') {
            return Object.assign({}, cmd, {command: cmd.command.explain});
        } else {
            const nestedCmd = cmd.command;
            delete nestedCmd.explain;
            return Object.assign({}, cmd, {command: nestedCmd});
        }
    }

    // The failpoints specified in the test case should ensure planningTimeMicros is at least
    // 'minPlanningTimeMicros'. The upper bound check is less important here; set a high
    // 'maxPlanningTimeMicros' to give lots of room for multiplanning, etc.
    const minPlanningTimeMicros = waitTimeMillis * 1000;
    const maxPlanningTimeMicros = minPlanningTimeMicros * 5;

    for (let explainTestCase of explainTestCases) {
        const testCase = unwrapExplain(explainTestCase);
        testProfileEntryContainsPlanningTime(db,
                                             testCase,
                                             1 /* expectedProfileEntries */,
                                             minPlanningTimeMicros,
                                             maxPlanningTimeMicros);
    }

    // Some final test cases that are not valid to run as explain commands, since they contain
    // multiple sub-operations. We should see two profile entries, one for each sub-operation.
    const filter = {a: "abc", b: "def", c: {$lt: 0}};
    const numExpectedEntries = 2;
    testProfileEntryContainsPlanningTime(
        db,
        {
            testName: "delete with multiple sub-operations",
            command: {delete: collName, deletes: [{q: filter, limit: 0}, {q: filter, limit: 0}]},
            failpointName: "sleepWhileMultiplanning",
            failpointOpts: {ms: waitTimeMillis},
        },
        numExpectedEntries,
        minPlanningTimeMicros,
        maxPlanningTimeMicros);
    testProfileEntryContainsPlanningTime(
        db,
        {
            testName: "update with multiple sub-operations",
            command: {
                update: collName,
                updates: [{q: filter, u: {$inc: {c: 1}}}, {q: filter, u: {$inc: {c: 1}}}]
            },
            failpointName: "sleepWhileMultiplanning",
            failpointOpts: {ms: waitTimeMillis},
        },
        numExpectedEntries,
        minPlanningTimeMicros,
        maxPlanningTimeMicros);
    testProfileEntryContainsPlanningTime(
        db,
        {
            testName: "bulkWrite with multiple sub-operations",
            command: {
                bulkWrite: 1,
                ops: [
                    {update: 0, filter: filter, updateMods: {$inc: {c: 1}}},
                    {delete: 0, filter: filter, multi: false}
                ],
                nsInfo: [{ns: ns}],
            },
            failpointName: "sleepWhileMultiplanning",
            failpointOpts: {ms: waitTimeMillis},
        },
        numExpectedEntries,
        minPlanningTimeMicros,
        maxPlanningTimeMicros);
}

jsTestLog("Testing standalone");
(function testStandalone() {
    const conn = MongoRunner.runMongod();
    const db = conn.getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        MongoRunner.stopMongod(conn);
    }
})();

jsTestLog("Testing replica set");
(function testReplicaSet() {
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        rst.stopSet();
    }
})();

(function testShardedCluster() {
    const st = new ShardingTest({shards: 2, mongos: 1, config: 1, other: {enableBalancer: false}});
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    const db = st.s.getDB(dbName);
    try {
        //
        // Run the same test cases as above.
        //
        jsTestLog("Testing on sharded cluster");
        runTest(db);

        //
        // Now test to ensure planningTimeMicros is reset when a query is retried.
        //
        jsTestLog("Test optimization stats do not include sharding refreshes");
        assert(db.getCollection(collName).drop());

        // Disable checking for index consistency to ensure that the config server doesn't trigger a
        // StaleShardVersion exception on shards and cause them to refresh their sharding metadata.
        // Also disable the best-effort recipient metadata refresh after migrations.
        st.forEachConfigServer(conn => {
            conn.adminCommand({setParameter: 1, enableShardedIndexConsistencyCheck: false});
        });
        FixtureHelpers.mapOnEachShardNode({
            db: db.getSiblingDB("admin"),
            func: (db) => configureFailPoint(
                db, "migrationRecipientFailPostCommitRefresh", {mode: "alwaysOn"}),
            primaryNodeOnly: true,
        });

        // Shard the collection and create two chunks. These both initially start on shard0.
        assert.commandWorked(st.s.getDB(dbName)[collName].insertMany(
            [{_id: -5}, {_id: 5}], {writeConcern: {w: "majority"}}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

        // Move a chunk from Shard0 to Shard1 through the main mongos, run a query, and then move
        // the chunk back. The end result: the primary is stale but not the router.
        const findCommand = {find: collName, filter: {_id: 5}};
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
        assert.commandWorked(db.runCommand(findCommand));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard0.shardName}));

        // Targets Shard0, which is stale and will need to do a refresh. The query will be retried
        // after the refresh. Assert that the planning time measurement includes exactly one
        // of the failpoint durations, not two (which would indicate that we're including the time
        // taken before the query was retried).
        const waitTimeMillis = 500;
        const minPlanningTimeMicros = waitTimeMillis * 1000;
        const maxPlanningTimeMicros = 2 * minPlanningTimeMicros;
        testProfileEntryContainsPlanningTime(db,
                                             {
                                                 testName: "find with stale shard",
                                                 command: findCommand,
                                                 failpointName: "setAutoGetCollectionWait",
                                                 failpointOpts: {waitForMillis: waitTimeMillis}
                                             },
                                             1 /* numExpectedProfileEntries */,
                                             minPlanningTimeMicros,
                                             maxPlanningTimeMicros);
    } finally {
        st.stop();
    }
})();
