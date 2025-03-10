
/**
 * Tests that a change stream will emit only a single 'dropDatabase' event in a sharded cluster.
 * @tags: [
 *   requires_fcv_81,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {assertChangeStreamEventEq} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 3,
    rs: {
        nodes: 1,
        setParameter: {
            writePeriodicNoops: true,
            periodicNoopIntervalSecs: 1,
        }
    }
});

function assertNextChangeStreamEventEquals(cursor, event) {
    assert.soon(() => cursor.hasNext());
    assertChangeStreamEventEq(cursor.next(), event);
}

// Create database with a sharded collection (3 shards).
const db = st.s.getDB(jsTestName());
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

const collName = "change_stream_single_drop_database_event";
assertDropAndRecreateCollection(db, collName);
const kNsName = jsTestName() + "." + collName;
assert.commandWorked(st.s0.adminCommand({shardCollection: kNsName, key: {_id: 1}}));
assert.commandWorked(st.s0.adminCommand({split: kNsName, middle: {_id: 10}}));
assert.commandWorked(st.s0.adminCommand({split: kNsName, middle: {_id: 20}}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: kNsName, find: {_id: 5}, to: st["shard0"].shardName}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: kNsName, find: {_id: 15}, to: st["shard1"].shardName}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: kNsName, find: {_id: 25}, to: st["shard2"].shardName}));

for (let i = 0; i <= 30; i += 5) {
    assert.commandWorked(db[collName].insert({_id: i}));
}

// Create changestream on the entire cluster.
const wholeClusterCursor = db.getMongo().watch([], {showExpandedEvents: true});

// Create changestream on the target database.
const databaseCursor = db.watch([], {showExpandedEvents: true});

// Drop the database.
assert.commandWorked(db.dropDatabase());

function testCursor(cursor, expectedEvents, label) {
    jsTestLog("Testing " + label);
    try {
        expectedEvents.forEach((event) => {
            assertNextChangeStreamEventEquals(cursor, event);
        });

        assert(!cursor.hasNext(), () => {
            return "Unexpected change set: " + tojson(cursor.toArray());
        });
    } finally {
        cursor.close();
    }
}

let expectedEvents = [
    {
        operationType: "drop",
        ns: {db: jsTestName(), coll: collName},
    },
    {
        operationType: "dropDatabase",
        ns: {db: jsTestName()},
    },
];

// Check expected events for whole cluster change stream.
testCursor(wholeClusterCursor, expectedEvents, "whole cluster cursor");

// Check expected events for single-database change stream.
// As the target database is dropped here, we need to add an invalidate event.
expectedEvents.push({operationType: "invalidate"});
testCursor(databaseCursor, expectedEvents, "database cursor");

st.stop();
