/**
 * This test stresses sharded $lookup using two threads. One moves chunks on a sharded collection,
 * the other issues $lookups from a local unsharded collection to the foreign sharded collection.
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

(function() {
"use strict";
const dbName = "testDb";
const collNameSampledSharded = "sampledCollSharded";
const sampledNsSharded = dbName + "." + collNameSampledSharded;
const collNameNotSampled = "notSampledColl";

const st = new ShardingTest({mongos: 2, shards: 3, rs: {nodes: 2}});
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const mongosDB = st.s.getDB(dbName);
assert.commandWorked(st.s.adminCommand({shardCollection: sampledNsSharded, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: sampledNsSharded, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({split: sampledNsSharded, middle: {x: 1000}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: sampledNsSharded, find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: sampledNsSharded, find: {x: 1000}, to: st.shard2.name}));
assert.commandWorked(mongosDB.getCollection(collNameNotSampled).insert([{a: 0}]));

// Repeatedly issues 'targetNumPerSec' $lookups for 'durationMs'.
function runNestedAggregateCmdsOnRepeat(
    mongosHost, dbName, localCollName, foreignCollName, targetNumPerSec, durationMs) {
    const mongos = new Mongo(mongosHost);
    const db = mongos.getDB(dbName);
    const cmdObj = {
        aggregate: localCollName,
        pipeline: [
            {$lookup: {from: foreignCollName, as: "joined", pipeline: [{$match: {x: {$gte: 0}}}]}}
        ],
        cursor: {},
    };
    const periodMs = 1000.0 / targetNumPerSec;
    const startTimeMs = Date.now();
    while (Date.now() - startTimeMs < durationMs) {
        const cmdStartTimeMs = Date.now();
        assert.commandWorked(db.runCommand(cmdObj));
        const cmdEndTimeMs = Date.now();
        sleep(Math.max(0, periodMs - (cmdEndTimeMs - cmdStartTimeMs)));
    }
}

function stressShardedLookup(dbName, collNameNotSampled, collNameSampled) {
    const durationSecs = 10;
    const durationMs = 1000 * durationSecs;
    const targetNumAggPerSec = 10;
    const aggThread = new Thread(runNestedAggregateCmdsOnRepeat,
                                 st.s0.host,
                                 dbName,
                                 collNameNotSampled,
                                 collNameSampled,
                                 targetNumAggPerSec,
                                 durationMs);
    aggThread.start();

    // Main thread moves chunks on the sharded collection.
    let shardNames = [st.shard0.name, st.shard1.name];
    let i = 0;
    const startTimeMs = Date.now();
    while (Date.now() - startTimeMs < durationMs) {
        assert.commandWorked(
            st.s1.adminCommand({moveChunk: sampledNsSharded, find: {x: 0}, to: shardNames[i % 2]}));
        i++;
    }
    aggThread.join();
}

stressShardedLookup(dbName, collNameNotSampled, collNameSampledSharded);
st.stop();
})();