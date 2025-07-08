/**
 * Tests server does not crash when attempting to delete a very large document via the batched
 * delete interface.
 * @tags: [requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
function runTestOnFixtures(runQueriesAndCompareResults) {
    {
        jsTestLog("Standalone to ReplSet migration test");
        const minIdErrorLen = 2 ** 24 - 14;

        // Start standalone and insert large document.
        const conn = MongoRunner.runMongod();
        const testDb = conn.getDB("test");
        assert.commandWorked(testDb.c.insert({_id: "X".repeat(minIdErrorLen - 1)}));
        const dbpath = conn.dbpath;
        const port = conn.port;
        MongoRunner.stopMongod(conn, null, {noCleanData: true});

        // Restart as ReplSetTest and delete the document.
        const rst = new ReplSetTest({nodes: 1, name: "rs0", nodeOptions: {dbpath: dbpath}});
        rst.startSet({noCleanData: true});
        rst.initiate();
        const replTestDb = rst.getPrimary().getDB("test");
        assert.eq(replTestDb.c.count(), 1);
        const explainResult = replTestDb.c.explain().remove({});
        const stage = explainResult.queryPlanner.winningPlan.stage;
        assert(stage === "BATCHED_DELETE");
        assert.commandWorked(replTestDb.c.remove({}));
        rst.stopSet();
    }
    {
        jsTestLog("Standalone");
        const conn = MongoRunner.runMongod();
        runQueriesAndCompareResults(conn, true);
        MongoRunner.stopMongod(conn);
    }
    {
        jsTestLog("ReplSetTest");
        const rst = new ReplSetTest({nodes: 3});
        rst.startSet();
        rst.initiate();
        runQueriesAndCompareResults(rst.getPrimary(), false);
        rst.stopSet();
    }
    {
        jsTestLog("ShardingTest");
        const st = new ShardingTest(Object.assign({shards: 2}));
        const testDB = st.s.getDB("test");
        assert.commandWorked(testDB.adminCommand(
            {enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));
        runQueriesAndCompareResults(st.s, false);
        st.stop();
    }
}

function tests(conn, isStandalone) {
    const testDb = conn.getDB("test");
    assert(testDb.c.drop());
    const minIdErrorLen = 2 ** 24 - 14;
    const n = 10;
    for (let idLen of [16776704, minIdErrorLen - 1, minIdErrorLen, minIdErrorLen + 1]) {
        for (let r = 1; r < n; r++) {
            assert.commandWorked(testDb.c.insert({_id: "X".repeat(r)}));
        }
        const res = testDb.c.insert({_id: "X".repeat(idLen)});
        for (let r = n + 1; r < 2 * n; r++) {
            assert.commandWorked(testDb.c.insert({_id: "X".repeat(r)}));
        }
        if (idLen >= minIdErrorLen) {
            assert.commandFailedWithCode(res, ErrorCodes.BadValue);
        } else {
            assert.commandWorked(res);
        }
        const explainResult = testDb.c.explain().remove({});
        const stage = explainResult.queryPlanner.winningPlan.stage;
        const shards = explainResult.queryPlanner.winningPlan.shards;  // May be undefined.
        jsTestLog(explainResult.queryPlanner.winningPlan);
        assert((stage === "BATCHED_DELETE") ||
               (stage === "SHARD_WRITE" && shards[0].winningPlan.stage === "BATCHED_DELETE"));
        assert.commandWorked(testDb.c.remove({}));
        assert.eq(testDb.c.find().toArray(), []);
    }
}

runTestOnFixtures(tests);
