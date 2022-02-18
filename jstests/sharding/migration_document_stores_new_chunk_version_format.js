/**
 * Checks the new persisted format of the chunk version ine th migration document.
 *
 * @tags: [
 *  multiversion_incompatible,
 *  featureFlagNewPersistedChunkVersionFormat
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');

const st = new ShardingTest({mongos: 1, shards: 2, config: 1});

const shard0Name = st.shard0.shardName;
const shard1Name = st.shard1.shardName;
const host = st.s0.host;

const dbName = 'foo';
const nss = dbName + '.' +
    'test';

st.s.adminCommand({enableSharding: dbName, primaryShard: shard0Name});
st.s.adminCommand({shardCollection: nss, key: {x: 1}});

// Stop the migration after persisting the document.
let fp = configureFailPoint(st.rs0.getPrimary(), 'moveChunkHangAtStep3');

let moveChunkThread = new Thread((mongosConnString, nss, shard1Name) => {
    var mongos = new Mongo(mongosConnString);
    mongos.adminCommand({moveChunk: nss, find: {x: 0}, to: shard1Name});
}, host, nss, shard1Name);

moveChunkThread.start();

fp.wait();
let migrationDoc = st.rs0.getPrimary().getDB('config').migrationCoordinators.findOne();

assert(migrationDoc.preMigrationChunkVersion.hasOwnProperty('v'));
assert(migrationDoc.preMigrationChunkVersion.hasOwnProperty('t'));
assert(migrationDoc.preMigrationChunkVersion.hasOwnProperty('e'));

fp.off();
moveChunkThread.join();
st.stop();
})();
