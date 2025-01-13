/**
 * Tests that change streams do not output an event for the _shardsvrDropCollectionIfUUIDNotMatching
 * command performed by resharding cleanup.
 */

function getCollectionUuid(conn, dbName, collName) {
    const listCollectionRes = assert.commandWorked(
        conn.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}}));
    return listCollectionRes.cursor.firstBatch[0].info.uuid;
}

/**
 * Returns events up until the given timestamp.
 */
function getEvents(cursor, minTimestamp) {
    const events = [];
    let lastEvent;
    do {
        assert.soon(() => cursor.hasNext(), "change stream timed out", 10 * 1000);
        lastEvent = cursor.next();
        events.push(lastEvent);
    } while (timestampCmp(minTimestamp, lastEvent.clusterTime) > 0);
    return events;
}

function assertReshardCollectionEvent(event, dbName, collName, collUuid) {
    assert.eq(event.operationType, "reshardCollection", event);
    assert.eq(event.hasOwnProperty("ns"), true, event);
    assert.eq(event.ns.db, dbName, event);
    assert.eq(event.ns.coll, collName, event);
    assert.eq(event.collectionUUID, collUuid, event);
}

function assertInsertEvent(event, dbName, collName, collUuid, doc) {
    assert.eq(event.operationType, "insert", event);
    assert.eq(event.hasOwnProperty("ns"), true, event);
    assert.eq(event.ns.db, dbName, event);
    assert.eq(event.ns.coll, collName, event);
    if (collUuid) {
        assert.eq(event.collectionUUID, collUuid, event);
    } else {
        assert.eq(event.hasOwnProperty("collectionUUID"), false, event);
    }
    assert.eq(event.fullDocument, doc, event);
}

const rsOptions = {
    setParameter: {
        periodicNoopIntervalSecs: 1,
        writePeriodicNoops: true,
    }
};
const st = new ShardingTest({
    shards: 2,
    rs: rsOptions,
    configOptions: rsOptions,
});

function runTest(testName, setUpFunc, runCmdFunc) {
    const testDbName = "testDb-" + testName;
    const testCollName = "testColl";

    const testDB = st.s.getDB(testDbName);
    const testColl = testDB.getCollection(testCollName);
    const adminDB = st.s.getDB("admin");

    setUpFunc(st, testDbName, testCollName);

    const numDocs = 10;
    for (let i = 0; i < numDocs / 2; i++) {
        assert.commandWorked(testColl.insert([
            {_id: UUID(), x: -i, y: -i},
            {_id: UUID(), x: i, y: i},
        ]));
    }

    const defaultOptions = {};
    const expandedOptions = {showExpandedEvents: true};

    const collectionChangeStreamCursorDefault =
        testColl.aggregate([{$changeStream: defaultOptions}]);
    const collectionChangeStreamCursorExpanded =
        testColl.aggregate([{$changeStream: expandedOptions}]);

    const databaseChangeStreamCursorDefault = testDB.aggregate([{$changeStream: defaultOptions}]);
    const databaseChangeStreamCursorExpanded = testDB.aggregate([{$changeStream: expandedOptions}]);

    const clusterChangeStreamCursorDefault = adminDB.aggregate(
        [{$changeStream: Object.assign({allChangesForCluster: true}, defaultOptions)}]);
    const clusterChangeStreamCursorExpanded = adminDB.aggregate(
        [{$changeStream: Object.assign({allChangesForCluster: true}, expandedOptions)}]);

    const collUuidBefore = getCollectionUuid(st.s, testDbName, testCollName);
    runCmdFunc(st, testDbName, testCollName);
    const collUuidAfter = getCollectionUuid(st.s, testDbName, testCollName);

    const newDoc = {_id: UUID(), x: 100, y: 100};
    const res =
        assert.commandWorked(testDB.runCommand({insert: testCollName, documents: [newDoc]}));
    const insertTs = res.operationTime;

    const validateEvents = (events, isExpanded) => {
        if (isExpanded) {
            assert.eq(events.length, 2, events);
            assertReshardCollectionEvent(events[0], testDbName, testCollName, collUuidBefore);
            assertInsertEvent(events[1], testDbName, testCollName, collUuidAfter, newDoc);
        } else {
            assert.eq(events.length, 1, events);
            assertInsertEvent(events[0], testDbName, testCollName, null /* collUuid */, newDoc);
        }
    };

    {
        const events = getEvents(collectionChangeStreamCursorDefault, insertTs);
        jsTest.log("The events in the collection change stream (default): " + tojson(events));
        validateEvents(events, false /* isExpanded */);
    }

    {
        const events = getEvents(collectionChangeStreamCursorExpanded, insertTs);
        jsTest.log("The events in the collection change stream (expanded): " + tojson(events));
        validateEvents(events, true /* isExpanded */);
    }

    {
        const events = getEvents(databaseChangeStreamCursorDefault, insertTs);
        jsTest.log("The events in the database change stream (default): " + tojson(events));
        validateEvents(events, false /* isExpanded */);
    }

    {
        const events = getEvents(databaseChangeStreamCursorExpanded, insertTs);
        jsTest.log("The events in the database change stream (expanded): " + tojson(events));
        validateEvents(events, true /* isExpanded */);
    }

    {
        const events = getEvents(clusterChangeStreamCursorDefault, insertTs);
        jsTest.log("The events in the cluster change stream (default): " + tojson(events));
        validateEvents(events, false /* isExpanded */);
    }

    {
        const events = getEvents(clusterChangeStreamCursorExpanded, insertTs);
        jsTest.log("The events in the cluster change stream (expanded): " + tojson(events));
        validateEvents(events, true /* isExpanded */);
    }
}

// Set up the test collection such that there is a shard where the
// _shardsvrDropCollectionIfUUIDNotMatching command performed by resharding cleanup is not a no-op.
// That is, make the cluster have a shard (namely shard0) that satisfies the following:
// 1. The shard is not the primary shard for the collection.
// 2. Before resharding, the shard does not own any chunks for that collection but the
//    collection exists locally on that shard (as an empty collection).
// 3. After resharding, the shard does not own any chunks for that collection.
// In other words, shard0 is neither a donor nor a recipient for the resharding operation.

// The collection must be sharded because:
// - Unsharded collections can only be moved using moveCollection (resharding) which always drops
//   the original collection from all shards.
// - Only sharded collections can be moved incrementally using chunk migrations which do not drop
//   the collection from the donor shard upon moving the last chunk.
// - To go from sharded to unsharded, a collection must go through unshardCollection (resharding)
//   which again drops the original collection from all shards.
// For this reason, this test scenario is only applicable to the reshardCollection and
// unshardCollection commands.

const testCases = [];

// To achieve (1) and (2):
// - Make the test database have shard1 as the primary shard.
// - Shard the test collection and move a chunk from shard1 to shard0 and back to shard1 so the
//   collection exists as an empty collection on shard0.
const setUp = (st, testDbName, testCollName) => {
    const testNs = testDbName + "." + testCollName;
    assert.commandWorked(
        st.s.adminCommand({enableSharding: testDbName, primaryShard: st.shard1.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: testNs, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: testNs, middle: {x: 0}}));

    assert.commandWorked(st.s.adminCommand(
        {moveChunk: testNs, find: {x: MinKey}, to: st.shard0.shardName, _waitForDelete: true}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: testNs, find: {x: MinKey}, to: st.shard1.shardName, _waitForDelete: true}));
};

// To achieve (3):
// - For reshardCollection, specify "zones" to make shard1 own all chunks.
// - For unshardCollection, specify shard1 as the destination shard.
const runReshardCollection = (st, testDbName, testCollName) => {
    const testNs = testDbName + "." + testCollName;

    const zoneName = "zoneA";
    assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: zoneName}));
    assert.commandWorked(st.s.adminCommand({
        reshardCollection: testNs,
        numInitialChunks: 1,
        key: {x: 1, y: 1},
        zones: [{min: {x: MinKey, y: MinKey}, max: {x: MaxKey, y: MaxKey}, zone: zoneName}]
    }));
};
const runUnshardCollection = (st, testDbName, testCollName) => {
    const testNs = testDbName + "." + testCollName;
    assert.commandWorked(
        st.s.adminCommand({unshardCollection: testNs, toShard: st.shard1.shardName}));
};

testCases.push({
    testName: "reshardCollection",
    setUpFunc: setUp,
    runCmdFunc: runReshardCollection,
});

// unshardCollection was introduced in 8.0.
const isMultiversion =
    jsTest.options().shardMixedBinVersions || jsTest.options().useRandomBinVersionsWithinReplicaSet;
if (!isMultiversion) {
    testCases.push({
        testName: "unshardCollection",
        setUpFunc: setUp,
        runCmdFunc: runUnshardCollection,
    });
}

for (let testCase of testCases) {
    jsTest.log("Testing case: " + testCase.testName);
    runTest(testCase.testName, testCase.setUpFunc, testCase.runCmdFunc);
}

st.stop();
