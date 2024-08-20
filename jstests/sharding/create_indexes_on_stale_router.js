/**
 * Tests createIndexes in a stale shard and stale db router
 * @tags: [
 *   multiversion_incompatible,
 *   requires_fcv_73,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = 'testDB';
const collName = 'testColl';
const bucketsCollName = 'system.buckets.' + collName;
const timeField = 'time';
const metaField = 'hostid';
const shardKey = {
    [metaField]: 1
};

const createIndexOnStaleRouter = (staleRouter) => {
    let cmdObj = {
        createIndexes: collName,
        indexes: [{key: {[timeField]: 1}, name: "index_on_time"}]
    };
    assert.commandWorked(staleRouter.runCommand(cmdObj));
};

const st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 2}});
const mongos0 = st.s0.getDB(dbName);
const mongos1 = st.s1.getDB(dbName);
const shard0DB = st.shard0.getDB(dbName);
const shard1DB = st.shard1.getDB(dbName);

// Insert some dummy data using 'mongos1' as the router, so that the cache is
// initialized on the mongos while the collection is unsharded.
assert.commandWorked(
    mongos1.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 1}}));
assert.commandWorked(mongos1.getCollection(collName).insert({_id: "aaa"}));

// Drop and shard the collection with 'mongos0' as the router.
assert(mongos0.getCollection(collName).drop());
assert.commandWorked(mongos0.adminCommand({
    shardCollection: `${dbName}.${collName}`,
    key: shardKey,
    timeseries: {timeField: timeField, metaField: metaField}
}));

createIndexOnStaleRouter(mongos1);

// Drop the database and create a new one.
assert.commandWorked(st.s0.getDB(dbName).dropDatabase());
assert.commandWorked(
    mongos0.adminCommand({shardCollection: `${dbName}.${collName}`, key: shardKey}));

createIndexOnStaleRouter(mongos1);
st.stop();
