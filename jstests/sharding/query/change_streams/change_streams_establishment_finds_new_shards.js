// Tests that change streams is able to find and return results from new shards which are added
// during cursor establishment.
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const rsNodeOptions = {
    // Use a higher frequency for periodic noops to speed up the test.
    setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true},
};
const st = new ShardingTest({shards: 1, mongos: 1, rs: {nodes: 1}, other: {rsOptions: rsNodeOptions}});

jsTestLog("Starting new shard (but not adding to shard set yet)");
const newShard = new ReplSetTest({name: "newShard", nodes: 1, nodeOptions: rsNodeOptions});
newShard.startSet({shardsvr: ""});
newShard.initiate();

const mongos = st.s;
const mongosColl = mongos.getCollection("test.foo");
const mongosDB = mongos.getDB("test");

// Enable sharding to inform mongos of the database, allowing us to open a cursor.
assert.commandWorked(mongos.adminCommand({enableSharding: mongosDB.getName()}));

// Shard the collection.
assert.commandWorked(mongos.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

// Split the collection into two chunks: [MinKey, 10) and [10, MaxKey].
assert.commandWorked(mongos.adminCommand({split: mongosColl.getFullName(), middle: {_id: 10}}));

// Enable the failpoint.
assert.commandWorked(
    mongos.adminCommand({configureFailPoint: "shardedAggregateHangBeforeEstablishingShardCursors", mode: "alwaysOn"}),
);

// While opening the cursor, wait for the failpoint and add the new shard.
const awaitNewShard = startParallelShell(
    `
        checkLog.contains(db,
            "shardedAggregateHangBeforeEstablishingShardCursors fail point enabled");
        assert.commandWorked(
            db.adminCommand({addShard: "${newShard.getURL()}", name: "${newShard.name}"}));
        // Migrate the [10, MaxKey] chunk to "newShard".
        assert.commandWorked(db.adminCommand({moveChunk: "${mongosColl.getFullName()}",
                                              find: {_id: 20},
                                              to: "${newShard.name}",
                                              _waitForDelete: true}));
        assert.commandWorked(
            db.adminCommand(
                {configureFailPoint: "shardedAggregateHangBeforeEstablishingShardCursors",
                 mode: "off"}));`,
    mongos.port,
);

jsTestLog("Opening $changeStream cursor");
const changeStream = mongosColl.aggregate([{$changeStream: {}}]);
assert(!changeStream.hasNext(), "Do not expect any results yet");

// Clean up the parallel shell.
awaitNewShard();

// Insert two documents in different shards.
assert.commandWorked(mongosColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));
assert.commandWorked(mongosColl.insert({_id: 20}, {writeConcern: {w: "majority"}}));

// Expect to see them both.
for (let id of [0, 20]) {
    jsTestLog("Expecting Item " + id);
    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.documentKey, {_id: id});
}
assert(!changeStream.hasNext());

st.stop();
newShard.stopSet();
