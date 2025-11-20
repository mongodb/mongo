/**
 * Tests the behavior of change streams error handling.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_83,
 *  requires_persistence,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ChangeStreamTest, isResumableChangeStreamError} from "jstests/libs/query/change_stream_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

// Simplify the comparison for the events received. Match only the fields present in expected.
function filterToExpectedKeys(event, expected) {
    const filteredEvent = {};
    for (const key in expected) {
        if (event.hasOwnProperty(key)) {
            filteredEvent[key] = event[key];
        }
    }
    return filteredEvent;
}

// Verify the set of events generated for the changestream.
function verifyEvents({changeStream, cursor, isCollectionChangeStream = false, endOfTransactionChangeEvent = false}) {
    let expectedOperationTypes = [
        "create",
        "shardCollection",
        "createIndexes",
        "insert",
        "insert",
        "create",
        "shardCollection",
        "insert",
        "insert",
        "endOfTransaction",
        "startIndexBuild",
        "createIndexes",
        "reshardBlockingWrites",
        "reshardCollection",
    ];

    if (isCollectionChangeStream || !endOfTransactionChangeEvent) {
        expectedOperationTypes = expectedOperationTypes.filter((event) => event !== "endOfTransaction");

        if (isCollectionChangeStream) {
            const startIndex = 5;
            const nElements = 6;
            // For collection events we receive only: [create, shardCollection, createIndexes, insert, insert, reshardBlockingWrites, reshardCollection].
            expectedOperationTypes.splice(startIndex, nElements);
        }
    }

    const expectedChanges = expectedOperationTypes.map((opType) => {
        return {operationType: opType};
    });

    changeStream.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: expectedChanges,
    });
    return true;
}

// Kills all shards and verifies that we throw an error associated to the cs cursor.
function killShardAndTestErrorForChangeStream({sharding, changeStream, cursor}) {
    sharding.stopAllShards(); // Stop all shards.

    let infoMessage = "";
    assert.soon(() => {
        try {
            // Attempt to read from the cursor, if the shard has been killed an exception will be thrown.
            changeStream.getNextBatch(cursor);
            infoMessage = "Unexpected. Change stream should have no events left to process.";
            return false;
        } catch (e) {
            // Log the error message received, which should state that the cursor cannot be read.
            jsTest.log.info("Cursor threw an error", {exception: e});
            // Most importantly verify that the error is resumable.
            return isResumableChangeStreamError(e);
        }
    }, "Test Failed: " + infoMessage);

    sharding.restartAllShards(); // Restart shards.
}

function createChangeStream({db, coll, allCluster = false}) {
    const cst = new ChangeStreamTest(db, {eventModifier: filterToExpectedKeys});
    const cursor = cst.startWatchingChanges({
        pipeline: [
            {
                $changeStream: {
                    showExpandedEvents: true,
                    showSystemEvents: true,
                    allChangesForCluster: allCluster,
                },
            },
        ],
        collection: coll,
    });
    return [cst, cursor];
}

// Create a single-shard cluster for this test.
const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 3, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
});

const adminDB = st.s.getDB("admin");
const testDB = st.s.getDB(jsTestName());
const testColl = testDB[jsTestName()];

assertDropCollection(testDB, testColl.getName());

const [dbCS, dbCursor] = createChangeStream({db: testDB, coll: 1});
const [collCS, collCursor] = createChangeStream({db: testDB, coll: testColl.getName()});
const [clusterCS, allClusterCursor] = createChangeStream({db: adminDB, coll: 1, allCluster: true});

//Setup sharding using _id.
st.shardColl(testColl, {_id: 1}, false);

// Build an index and insert some documents.
assert.commandWorked(testColl.createIndex({a: 1}));
assert.commandWorked(testColl.insert({_id: 0, a: 0}));
assert.commandWorked(testColl.insert({_id: 1, a: 1}));

// Reshard the collection.
assert.commandWorked(
    st.s.adminCommand({
        reshardCollection: testColl.getFullName(),
        key: {a: 1},
        numInitialChunks: 1,
    }),
);

// Verify events for all changestreams.
const isEndOfTransactionEnabled = FeatureFlagUtil.isEnabled(st.s, "EndOfTransactionChangeEvent");
verifyEvents({
    changeStream: clusterCS,
    cursor: allClusterCursor,
    endOfTransactionChangeEvent: isEndOfTransactionEnabled,
});
verifyEvents({changeStream: dbCS, cursor: dbCursor, endOfTransactionChangeEvent: isEndOfTransactionEnabled});
verifyEvents({
    changeStream: collCS,
    cursor: collCursor,
    isCollectionChangeStream: true,
    endOfTransactionChangeEvent: isEndOfTransactionEnabled,
});

// Kill all shards and verify changestream errors.
killShardAndTestErrorForChangeStream({sharding: st, changeStream: clusterCS, cursor: allClusterCursor});
killShardAndTestErrorForChangeStream({sharding: st, changeStream: dbCS, cursor: dbCursor});
killShardAndTestErrorForChangeStream({sharding: st, changeStream: collCS, cursor: collCursor});

dbCS.cleanUp();
collCS.cleanUp();
clusterCS.cleanUp();
st.stop();
