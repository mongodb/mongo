/**
 * Tests readConcern level snapshot outside of transactions.
 *
 * @tags: [
 *   requires_fcv_46,
 *   requires_majority_read_concern,
 *   requires_find_command
 * ]
 */

(function() {
"use strict";

load("jstests/libs/global_snapshot_reads_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

// TODO(SERVER-47672): Use minSnapshotHistoryWindowInSeconds instead.
const configOptions = {
    setParameter: {maxTargetSnapshotHistoryWindowInSeconds: 600}
};

const dbName = "test";
const shardedCollName = "shardedColl";
const unshardedCollName = "unshardedColl";

function setUpAllScenarios(st) {
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand(
        {shardCollection: st.s.getDB(dbName)[shardedCollName] + "", key: {_id: 1}}));
}

let shardingScenarios = {
    singleShard: {
        compatibleCollections: [shardedCollName, unshardedCollName],
        setUp: function() {
            const st = new ShardingTest({
                mongos: 1,
                config: 1,
                shards: {rs0: {nodes: 2}},
                other: {configOptions: configOptions}
            });
            setUpAllScenarios(st);
            return st;
        }
    },
    multiShardAllShardReads: {
        compatibleCollections: [shardedCollName],
        setUp: function() {
            let st = new ShardingTest({
                shards: {
                    rs0: {nodes: 2},
                    rs1: {nodes: 2},
                    rs2: {nodes: 2},
                },
                mongos: 1,
                config: 1,
                other: {configOptions: configOptions}
            });
            setUpAllScenarios(st);
            const mongos = st.s0;
            const ns = dbName + '.' + shardedCollName;

            // snapshotReadsTest() inserts ids 0-9 and tries snapshot reads on the collection.
            assert.commandWorked(st.splitAt(ns, {_id: 4}));
            assert.commandWorked(st.splitAt(ns, {_id: 7}));

            assert.commandWorked(
                mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));
            assert.commandWorked(
                mongos.adminCommand({moveChunk: ns, find: {_id: 4}, to: st.shard1.shardName}));
            assert.commandWorked(
                mongos.adminCommand({moveChunk: ns, find: {_id: 7}, to: st.shard2.shardName}));

            assert.eq(1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard0.shardName}));
            assert.eq(1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard1.shardName}));
            assert.eq(1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard2.shardName}));

            flushRoutersAndRefreshShardMetadata(st, {ns});

            return st;
        }
    },
    // Only two out of three shards have documents.
    multiShardSomeShardReads: {
        compatibleCollections: [shardedCollName],
        setUp: function() {
            let st = new ShardingTest({
                shards: {
                    rs0: {nodes: 2},
                    rs1: {nodes: 2},
                    rs2: {nodes: 2},
                },
                mongos: 1,
                config: 1,
                other: {configOptions: configOptions}
            });
            setUpAllScenarios(st);
            const mongos = st.s0;
            const ns = dbName + '.' + shardedCollName;

            // snapshotReadsTest() inserts ids 0-9 and tries snapshot reads on the collection.
            assert.commandWorked(st.splitAt(ns, {_id: 5}));
            assert.commandWorked(
                mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
            assert.commandWorked(
                mongos.adminCommand({moveChunk: ns, find: {_id: 7}, to: st.shard2.shardName}));

            assert.eq(0, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard0.shardName}));
            assert.eq(1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard1.shardName}));
            assert.eq(1, mongos.getDB('config').chunks.count({ns: ns, shard: st.shard2.shardName}));

            flushRoutersAndRefreshShardMetadata(st, {ns});

            return st;
        }
    }
};

for (let [scenarioName, scenario] of Object.entries(shardingScenarios)) {
    scenario.compatibleCollections.forEach(function(collName) {
        jsTestLog(`Run scenario ${scenarioName} with collection ${collName}`);
        let st = scenario.setUp();

        function awaitCommittedFn() {
            for (let i = 0; st['rs' + i] !== undefined; i++) {
                st['rs' + i].awaitLastOpCommitted();
            }
        }

        // Pass the same DB handle as "primaryDB" and "secondaryDB" params; the test functions will
        // send readPreference to mongos to target primary/secondary shard servers.
        let db = st.s.getDB(dbName);
        snapshotReadsTest({
            testScenarioName: scenarioName,
            primaryDB: db,
            secondaryDB: db,
            collName: collName,
            awaitCommittedFn: awaitCommittedFn
        });
        st.stop();
    });
}
})();
