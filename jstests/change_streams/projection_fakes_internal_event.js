/**
 * Tests that a user projection which fakes an internal topology-change event is handled gracefully
 * in a sharded cluster.
 * TODO SERVER-65778: rework this test when we can handle faked internal events more robustly.
 *
 * Tests that if a user fakes an internal event with a projection nothing crashes, so not valuable
 * to test with a config shard.
 * @tags: [assumes_read_preference_unchanged, catalog_shard_incompatible]
 */
(function() {
"use strict";

const numShards = 2;

const st = new ShardingTest({
    shards: numShards,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

const mongosConn = st.s;

const testDB = mongosConn.getDB(jsTestName());
const adminDB = mongosConn.getDB("admin");
const testColl = testDB.test;

// Insert one test document that points to a valid shard, and one that points to an invalid shard.
// These will generate change events that look identical to a config.shards entry, except for 'ns'.
// It also means that the documentKey field in the resume token will look like a potentially valid
// new-shard document.
const existingShardDoc = testDB.getSiblingDB("config").shards.find({_id: st.rs0.name}).next();
const existingShardWrongNameDoc = {
    _id: "nonExistentName",
    host: existingShardDoc.host
};
const existingShardWrongHostDoc = {
    _id: st.rs1.name,
    host: `${st.rs1.name}/${st.rs1.host}-wrong:${st.rs1.ports[0]}`
};
const fakeShardDoc = {
    _id: "shardX",
    host: "shardX/nonExistentHost:27017"
};
const invalidShardDoc = {
    _id: "shardY",
    host: null
};
const configDotShardsNs = {
    db: "config",
    coll: "shards"
};
assert.commandWorked(testColl.insert(existingShardWrongNameDoc));
assert.commandWorked(testColl.insert(existingShardWrongHostDoc));
assert.commandWorked(testColl.insert(existingShardDoc));
assert.commandWorked(testColl.insert(invalidShardDoc));
assert.commandWorked(testColl.insert(fakeShardDoc));

// Log the shard description documents that we just inserted into the collection.
jsTestLog("Shard docs: " + tojson(testColl.find().toArray()));

// Helper function which opens a stream with the given projection and asserts that its behaviour
// conforms to the specified arguments; it will either throw the given error code, or return the
// expected events. Passing an empty array will confirm that we see no events in the stream. We
// further confirm that the faked events do not cause additional cursors to be opened.
function assertChangeStreamBehaviour(projection, expectedEvents, expectedErrorCode = null) {
    // Can't expect both to see events and to throw an exception.
    assert(!(expectedEvents && expectedErrorCode));

    // Generate a random ID for this stream.
    const commentID = `${Math.random()}`;

    // Create a change stream cursor with the specified projection.
    var csCursor = testColl.watch([{$addFields: projection}],
                                  {startAtOperationTime: Timestamp(1, 1), comment: commentID});

    // Confirm that the observed events match the expected events, if specified.
    if (expectedEvents && expectedEvents.length > 0) {
        for (let expectedEvent of expectedEvents) {
            assert.soon(() => csCursor.hasNext());
            const nextEvent = csCursor.next();
            for (let fieldName in expectedEvent) {
                assert.eq(
                    expectedEvent[fieldName], nextEvent[fieldName], {expectedEvent, nextEvent});
            }
        }
    }
    // If there are no expected events, confirm that the token advances without seeing anything.
    if (expectedEvents && expectedEvents.length == 0) {
        const startPoint = csCursor.getResumeToken();
        assert.soon(() => {
            assert(!csCursor.hasNext(), () => tojson(csCursor.next()));
            return bsonWoCompare(csCursor.getResumeToken(), startPoint) > 0;
        });
    }

    // If we expect an error code, assert that we throw it soon.
    if (expectedErrorCode) {
        assert.soon(() => {
            try {
                assert.throwsWithCode(() => csCursor.hasNext(), expectedErrorCode);
            } catch (err) {
                return false;
            }
            return true;
        });
    } else {
        // Otherwise, confirm that we still only have a single cursor on each shard. It's possible
        // that the same cursor will be listed as both active and inactive, so group by cursorId.
        const openCursors = adminDB
                                .aggregate([
                                    {$currentOp: {idleCursors: true}},
                                    {$match: {"cursor.originatingCommand.comment": commentID}},
                                    {
                                        $group: {
                                            _id: {shard: "$shard", cursorId: "$cursor.cursorId"},
                                            currentOps: {$push: "$$ROOT"}
                                        }
                                    }
                                ])
                                .toArray();
        assert.eq(openCursors.length,
                  numShards,
                  // Dump all the running operations for better debuggability.
                  () => tojson(adminDB.aggregate([{$currentOp: {idleCursors: true}}]).toArray()));
    }

    // Close the change stream when we are done.
    csCursor.close();
}

// Test that a projection which fakes a 'migrateChunkToNewShard' event is swallowed but has no
// effect.
let testProjection = {operationType: "migrateChunkToNewShard"};
assertChangeStreamBehaviour(testProjection, []);

// Test that a projection which fakes an event on config.shards with a non-string operationType is
// allowed to pass through.
testProjection = {
    ns: configDotShardsNs,
    operationType: null
};
assertChangeStreamBehaviour(testProjection, [
    {operationType: null, fullDocument: existingShardWrongNameDoc},
    {operationType: null, fullDocument: existingShardWrongHostDoc},
    {operationType: null, fullDocument: existingShardDoc},
    {operationType: null, fullDocument: invalidShardDoc},
    {operationType: null, fullDocument: fakeShardDoc}
]);

// Test that a projection which fakes an event on config.shards with a non-timestamp clusterTime
// is allowed to pass through.
testProjection = {
    ns: configDotShardsNs,
    clusterTime: null
};
assertChangeStreamBehaviour(testProjection, [
    {clusterTime: null, fullDocument: existingShardWrongNameDoc},
    {clusterTime: null, fullDocument: existingShardWrongHostDoc},
    {clusterTime: null, fullDocument: existingShardDoc},
    {clusterTime: null, fullDocument: invalidShardDoc},
    {clusterTime: null, fullDocument: fakeShardDoc}
]);

// Test that a projection which fakes an event on config.shards with a non-object fullDocument
// is allowed to pass through.
testProjection = {
    ns: configDotShardsNs,
    fullDocument: null
};
assertChangeStreamBehaviour(testProjection, [
    {fullDocument: null},
    {fullDocument: null},
    {fullDocument: null},
    {fullDocument: null},
    {fullDocument: null}
]);

// Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument
// pointing to an existing shard is swallowed but has no effect.
testProjection = {
    ns: configDotShardsNs,
    fullDocument: existingShardDoc
};
assertChangeStreamBehaviour(testProjection, []);

// Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument
// pointing to an existing shard's host, but the wrong shard name, throws as it attempts to connect.
testProjection = {
    ns: configDotShardsNs,
    fullDocument: existingShardWrongNameDoc
};
assertChangeStreamBehaviour(testProjection, null, ErrorCodes.ShardNotFound);

// Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument
// pointing to an existing shard's name, but the wrong host, is swallowed and has no effect.
testProjection = {
    ns: configDotShardsNs,
    fullDocument: existingShardWrongHostDoc
};
assertChangeStreamBehaviour(testProjection, []);

// Test that a projection which fakes a new-shard event on config.shards with a valid fullDocument
// pointing to a non-existent shard throws as it attempts to connect.
testProjection = {
    ns: configDotShardsNs,
    fullDocument: fakeShardDoc
};
assertChangeStreamBehaviour(testProjection, null, ErrorCodes.ShardNotFound);

// Test that a projection which fakes a new-shard event on config.shards with an invalid
// fullDocument throws a validation exception.
testProjection = {
    ns: configDotShardsNs,
    fullDocument: invalidShardDoc
};
assertChangeStreamBehaviour(testProjection, null, ErrorCodes.TypeMismatch);

st.stop();
})();
