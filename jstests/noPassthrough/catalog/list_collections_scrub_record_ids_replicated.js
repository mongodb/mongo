/**
 * Tests that 'recordIdsReplicated' is scrubbed from the 'info' sub-document of each collection
 * entry returned by listCollections when run via the router (mongos). The field is internal and
 * can be inconsistent across shards, so it must not be exposed to clients.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 *   expects_explicit_underscore_id_index,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 1});
const dbName = "test";
const collName = jsTestName();

const mongos = st.s0;
const shard0 = st.shard0;

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

const testDB = mongos.getDB(dbName);
assert.commandWorked(testDB.createCollection(collName));

// Verify the field is present when querying the shard directly.
const shardCollInfos = shard0.getDB(dbName).getCollectionInfos({name: collName});
assert.eq(1, shardCollInfos.length, "expected to find collection on shard: " + tojson(shardCollInfos));
const shardCollInfo = shardCollInfos[0];
assert(
    shardCollInfo.info.hasOwnProperty("recordIdsReplicated"),
    "expected 'recordIdsReplicated' to be present in shard listCollections response: " + tojson(shardCollInfo),
);

// Verify the field is scrubbed when querying via the router (mongos).
const routerCollInfos = testDB.getCollectionInfos({name: collName});
assert.eq(1, routerCollInfos.length, "expected to find collection via router: " + tojson(routerCollInfos));
const routerCollInfo = routerCollInfos[0];
assert(
    !routerCollInfo.info.hasOwnProperty("recordIdsReplicated"),
    "expected 'recordIdsReplicated' to be absent in router listCollections response: " + tojson(routerCollInfo),
);

st.stop();
