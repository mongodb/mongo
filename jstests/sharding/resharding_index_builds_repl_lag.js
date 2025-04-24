/**
 * Tests that resharding recipients wait for replication lag across all nodes to be below
 * reshardingWaitForReplicationTimeoutSeconds before building indexes, and that the wait can detect
 * and account for node removals.
 */
import {configureFailPoint} from 'jstests/libs/fail_point_util.js';
import {Thread} from 'jstests/libs/parallelTester.js';
import {ShardingTest} from 'jstests/libs/shardingtest.js';
import {extractUUIDFromObject, getUUIDFromListCollections} from 'jstests/libs/uuid_util.js';
import {stopServerReplication} from 'jstests/libs/write_concern_util.js';
import {reconfig} from 'jstests/replsets/rslib.js';

function runMoveCollection(host, ns, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveCollection: ns, toShard});
}

function indexExists(coll, indexKey) {
    const indexes = coll.getIndexes();
    return indexes.some(index => bsonWoCompare(index.key, indexKey) == 0);
}

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
        // Disallow chaining to force both secondaries to sync from the primary. One
        // of the test cases below disables replication on one of the secondaries,
        // with chaining that would effectively disable replication on both
        // secondaries, causing the test case to to fail since writeConcern of w:
        // majority is unsatisfiable.
        settings: {chainingAllowed: false},
    }
});

// Set up the collection to reshard.
const dbName0 = 'testDb0';
const collName0 = 'testColl0';
const ns0 = dbName0 + '.' + collName0;
const testColl0 = st.s.getCollection(ns0);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName0, primaryShard: st.shard0.shardName}));
assert.commandWorked(testColl0.insert([{x: -1}, {x: 0}, {x: 1}]));
assert.commandWorked(testColl0.createIndex({x: 1}));
const collUuid0 = getUUIDFromListCollections(st.s.getDB(dbName0), collName0);
const temporaryReshardingNs0 = `${dbName0}.system.resharding.${extractUUIDFromObject(collUuid0)}`;
const temporaryReshardingTestColl0 = st.s.getCollection(temporaryReshardingNs0);

// Set up the collection to write to in order to introduce replication lag on
// the recipient shard.
const dbName1 = 'testDb1';
const collName1 = 'testColl1';
const testDb1 = st.s.getDB(dbName1);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName1, primaryShard: st.shard1.shardName}));

const reshardingMaxReplicationLagSecondsBeforeBuildingIndexes = 1;
assert.commandWorked(st.rs1.getPrimary().adminCommand({
    setParameter: 1,
    reshardingMaxReplicationLagSecondsBeforeBuildingIndexes,
}));

const fp = configureFailPoint(st.rs1.getPrimary(), 'reshardingPauseRecipientBeforeBuildingIndex');

const moveThread = new Thread(runMoveCollection, st.s.host, ns0, st.shard1.shardName);
moveThread.start();
fp.wait();

// Pause replication on one of the secondaries.
stopServerReplication(st.rs1.nodes[2]);

// Perform a write and and wait for it to replicate to the other secondary.
sleep(reshardingMaxReplicationLagSecondsBeforeBuildingIndexes * 2000);
const res = assert.commandWorked(
    testDb1.runCommand({insert: collName1, documents: [{y: 0}], writeConcern: {w: 2}}));
jsTest.log("Performed insert: " + tojson(res));

fp.off();
jsTest.log("Verify that the resharding operation does not start building indexes since " +
           "the replication lag on one of the secondaries is above the configured threshold");
sleep(100);
// Verify that the index has not been created yet.
assert(!indexExists(temporaryReshardingTestColl0, {x: 1}));

st.rs1.remove(2);
const config = st.rs1.getReplSetConfigFromNode();
const newConfig = Object.assign({}, config);
newConfig.members = newConfig.members.slice(0, 2);  // Remove the last node.
reconfig(st.rs1, newConfig);

jsTest.log("Verify that the resharding operation starts building indexes since after the lagged " +
           "secondary has been removed, the replication lag is now the configured threshold");
assert.commandWorked(moveThread.returnData());
// Verify that the index was created.
assert(indexExists(testColl0, {x: 1}));

st.stop();
