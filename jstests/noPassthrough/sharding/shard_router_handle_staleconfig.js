/**
 * Tests that mongos can detect stale routing information before checking for UUID mismatches and
 * redirect the request to the appropriate shard.
 *
 * @tags: [requires_sharding]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 2});
const dbName = "db";

function checkCommand(cmd, collName, withinTransaction) {
    const db = st.getDB(dbName);
    const coll = db[collName];
    coll.drop();

    // Create a sharded collection and move it to the secondary shard.
    assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
    const nonPrimaryShard = st.getOther(st.getPrimaryShard(dbName)).name;
    assert.commandWorked(st.s0.adminCommand(
        {moveChunk: `${dbName}.${collName}`, find: {a: 0}, to: nonPrimaryShard}));
    // We now proceed to insert one document on each mongos connection. This will register cache
    // information about where to route the requests to that particular shard key.
    let i = 0;
    st.forEachMongos(mongos => {
        mongos.getDB(dbName)[collName].insert({a: 0, x: i++});
    });

    let session;
    if (withinTransaction) {
        session = st.s1.getDB(dbName).getMongo().startSession();
        session.startTransaction({readConcern: {level: "snapshot"}});
    }
    // Drop and recreate the collection on the primary shard. Now the collection resides on the
    // primary shard rather than the secondary. Note that we are only doing this in one mongos so
    // that the other one has stale information.
    const sDb = st.s0.getDB(dbName);
    assert.commandWorked(sDb.runCommand({drop: coll.getName()}));
    assert.commandWorked(sDb.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
    const newUuid = sDb.getCollectionInfos({name: coll.getName()})[0].info.uuid;

    // Proceed to make a request on the other mongos for the new collection. We expect this request
    // to get sent to the wrong shard as the router is stale. mongos should detect this and retry
    // the request with the correct shard. No exception should be passed to the user in this case.
    if (withinTransaction) {
        const sessionColl = session.getDatabase(dbName).getCollection(collName);
        assert.commandWorked(sessionColl.runCommand(Object.extend(cmd, {collectionUUID: newUuid})));
        session.commitTransaction();
    } else {
        assert.commandWorked(st.s1.getDB(dbName)[collName].runCommand(
            Object.extend(cmd, {collectionUUID: newUuid})));
    }
}

let collName = jsTestName() + "_find";
checkCommand({find: collName, filter: {}}, collName, false);
checkCommand({find: collName, filter: {}}, collName, true);
collName = jsTestName() + "_insert";
checkCommand({insert: collName, documents: [{x: 1}]}, collName, false);
checkCommand({insert: collName, documents: [{x: 1}]}, collName, true);
collName = jsTestName() + "_agg";
checkCommand(
    {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}}, collName, false);
checkCommand(
    {aggregate: collName, pipeline: [{$match: {x: 1}}], cursor: {batchSize: 10}}, collName, true);

st.stop();