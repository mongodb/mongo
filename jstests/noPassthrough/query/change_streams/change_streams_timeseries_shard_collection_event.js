/**
 * Validates the expected format of shardCollection events on timeseries collections,
 * given different varitions of creating and sharding commands.
 */

import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    assertChangeStreamEventEq,
    ChangeStreamTest
} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Test constants.
// All test variations use the same db and collection name.
const dbName = jsTestName();
const collName = "test_timeseries_coll";
const collNS = dbName + "." + collName;
const systemCollNS = "system.buckets." + collName;
const ns = {
    db: dbName,
    coll: systemCollNS
};

// Runs and end-to-end test that spins up a sharded testing cluster,
// creates a timeseries collection, shards it, and then asserts that the format of the associated
// 'shardCollection' event that is emitted is as expected.
// This allows us to quickly check that different variations of creating and sharding timeseries
// collections produces the correct 'shardCollection' event.
// Takes in 2 arguments:
// 1) st - an instance of ShardingTest that can be shared across calls.
// 2) params - follows the structure:
// {
//   createCollectionCommand: {
//     <second arg to db.createCollection()>
//   },
//   shardCollectionCommand: {
//     <arg to db.adminCommand() that will shard the collection>
//   },
//   expectedShardCollectionEventOperationDescription: {
//     <'operationDescription' field of 'shardCollection' event>
//   }
// }
function createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, params) {
    const mongosConn = st.s;
    const db = mongosConn.getDB(dbName);

    const test = new ChangeStreamTest(db);
    let cursor = test.startWatchingChanges({
        pipeline: [{$changeStream: {showExpandedEvents: true, showSystemEvents: true}}],
        collection: 1,
    });

    // Helper function that watches the change stream events, and waits until a 'shardCollection'
    // event is found. Upon finding this event, the structure of the event is validated.
    function findAndAssertShardCollectionEvent(expectedShardCollectionEvent) {
        let events = test.getNextChanges(cursor, 1);
        while (events.length > 0) {
            if (events[0].operationType == "shardCollection") {
                break;
            }
            // The only possible other events are create collection or index.
            assert(
                events[0].operationType == "create" || events[0].operationType == "createIndexes",
                "got event type other than 'create' or 'createIndexes'");
            events = test.getNextChanges(cursor, 1);
        }
        assert(events.length != 0, "no 'shardCollection' event found");

        let shardCollectionEvent = events[0];
        assertChangeStreamEventEq(shardCollectionEvent, expectedShardCollectionEvent);
    }

    // Create and shard the timeseries collection.
    assert.commandWorked(db.adminCommand({enableSharding: dbName}));
    assert.commandWorked(db.createCollection(collName, params.createCollectionCommand));
    assert.commandWorked(db.adminCommand(params.shardCollectionCommand));

    // Setup complete for this variation; assert 'shardCollection' event structure.
    findAndAssertShardCollectionEvent({
        "operationType": "shardCollection",
        ns,
        "operationDescription": params.expectedShardCollectionEventOperationDescription
    });

    // Clean up to re-use the sharding test.
    assertDropCollection(db, collName);
}

// Run test variations:
// 'presplitHashedZones' and 'capped' options are not supported on timeseries collections.
let timeFieldName = "timestamp";
let timeFieldShardKeyPrefix = "control.min.";
let timeFieldShardKey = timeFieldShardKeyPrefix + timeFieldName;

var st = new ShardingTest({
    shards: 2,
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}}
});

{
    // 'timeField' range-based sharding.
    // Hashed sharding is not supported on timeFields for timeseries collections.
    createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, {
        createCollectionCommand: {timeseries: {timeField: timeFieldName, metaField: "metadata"}},
        shardCollectionCommand: {shardCollection: collNS, key: {[timeFieldName]: 1}},
        expectedShardCollectionEventOperationDescription: {
            "shardKey": {[timeFieldShardKey]: 1},
            "unique": false,
            "presplitHashedZones": false,
            "capped": false
        }
    });

    // 'metaField' range-based sharding.
    createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, {
        createCollectionCommand: {timeseries: {timeField: timeFieldName, metaField: "metadata"}},
        shardCollectionCommand: {shardCollection: collNS, key: {metadata: 1}},
        expectedShardCollectionEventOperationDescription: {
            "shardKey": {"meta": 1},
            "unique": false,
            "presplitHashedZones": false,
            "capped": false
        }
    });

    // 'metaField' hashed-based sharding.
    createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, {
        createCollectionCommand: {timeseries: {timeField: timeFieldName, metaField: "metadata"}},
        shardCollectionCommand: {shardCollection: collNS, key: {metadata: "hashed"}},
        expectedShardCollectionEventOperationDescription: {
            "shardKey": {"meta": "hashed"},
            "unique": false,
            "presplitHashedZones": false,
            "capped": false
        }
    });

    // 'metaField' range-based compound key sharding.
    createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, {
        createCollectionCommand: {timeseries: {timeField: timeFieldName, metaField: "metadata"}},
        shardCollectionCommand: {shardCollection: collNS, key: {"metadata.x": 1, "metadata.y": 1}},
        expectedShardCollectionEventOperationDescription: {
            "shardKey": {"meta.x": 1, "meta.y": 1},
            "unique": false,
            "presplitHashedZones": false,
            "capped": false
        }
    });

    // 'metaField' mixed range and hash based compound key sharding.
    createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, {
        createCollectionCommand: {timeseries: {timeField: timeFieldName, metaField: "metadata"}},
        shardCollectionCommand:
            {shardCollection: collNS, key: {"metadata.x": 1, "metadata.y": "hashed"}},
        expectedShardCollectionEventOperationDescription: {
            "shardKey": {"meta.x": 1, "meta.y": "hashed"},
            "unique": false,
            "presplitHashedZones": false,
            "capped": false
        }
    });

    // 'metaField' mixed range and hash based compound (meta and time) key sharding.
    createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, {
        createCollectionCommand: {timeseries: {timeField: timeFieldName, metaField: "metadata"}},
        shardCollectionCommand: {
            shardCollection: collNS,
            key: {"metadata.x": 1, "metadata.y": "hashed", [timeFieldName]: 1}
        },
        expectedShardCollectionEventOperationDescription: {
            "shardKey": {"meta.x": 1, "meta.y": "hashed", [timeFieldShardKey]: 1},
            "unique": false,
            "presplitHashedZones": false,
            "capped": false
        }
    });

    // Testing unique shardKey option.
    createAndShardTimeseriesCollectionThenAssertShardCollectionFormat(st, {
        createCollectionCommand: {timeseries: {timeField: timeFieldName, metaField: "metadata"}},
        shardCollectionCommand: {shardCollection: collNS, key: {[timeFieldName]: 1}, unique: true},
        expectedShardCollectionEventOperationDescription: {
            "shardKey": {[timeFieldShardKey]: 1},
            "unique": true,
            "presplitHashedZones": false,
            "capped": false,
        }
    });
}

st.stop();
