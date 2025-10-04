/**
 * DDL coordinator are responsible for dropping temporary collections, especially after failures.
 * However, the change stream should not be aware of those events.
 * @tags: [
 *  # Requires all nodes to be running the latest binary.
 *  multiversion_incompatible,
 *  uses_change_streams,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertNoDrop(changeStream) {
    while (changeStream.hasNext()) {
        assert.neq(changeStream.next().operationType, "drop");
    }
}

function emptyChangeStream(changeStream) {
    while (changeStream.hasNext()) {
        changeStream.next();
    }
}

const dbName = "db";

// Enable explicitly the periodic no-op writer to allow the router to process change stream events
// coming from all shards. This is enabled for production clusters by default.
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {enableBalancer: true},
});

// create a database and a change stream on it
jsTest.log("Creating a change stream on " + dbName);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
let changeStream = st.s.getDB("db").watch();

// setFeatureCompatibilityVersion might cause dropping of deprecated collections
emptyChangeStream(changeStream);

jsTest.log(
    "The shard_collection_coordinator at second attempt (after failure) should not report drop events for orphaned",
);
{
    configureFailPoint(st.shard0, "failAtCommitCreateCollectionCoordinator", {}, {times: 1});

    let collectionName = dbName + ".coll";
    assert.commandWorked(st.s.adminCommand({shardCollection: collectionName, key: {_id: "hashed"}}));

    assertNoDrop(changeStream);
}

st.stop();
