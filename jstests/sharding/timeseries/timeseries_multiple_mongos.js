/**
 * Test various commands on time-series collection in the presence of multiple mongos and collection
 * changing from unsharded to sharded and vice versa.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRawOperationSpec, getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

Random.setRandomSeed();

const dbName = "testDB";
const collName = "testColl";
const timeField = "time";
const metaField = "hostid";
const testTimestamp = ISODate();

// Connections.
const st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 2}});
const mongos0 = st.s0.getDB(dbName);
const mongos1 = st.s1.getDB(dbName);

// Databases and collections.
assert.commandWorked(mongos0.adminCommand({enableSharding: dbName}));

/**
 * The test runs drop, create and shardCollection commands using mongos0, then validates the given
 * command by running against mongos1 with stale config.
 */
function runTest({shardKey, cmdObj}) {
    const shardKeyHasMetaField = shardKey[metaField] !== undefined;

    // Insert some dummy data using 'mongos1' as the router, so that the cache is initialized on the
    // mongos while the collection is unsharded.
    assert.commandWorked(mongos1.getCollection(collName).insert({[timeField]: ISODate()}));

    // Drop and shard the collection with 'mongos0' as the router.
    assert(mongos0.getCollection(collName).drop());
    assert.commandWorked(
        mongos0.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}),
    );
    assert.commandWorked(
        st.s0.adminCommand({
            shardCollection: `${dbName}.${collName}`,
            key: shardKey,
        }),
    );

    // Move one of the chunks into the second shard. Note that we can only do this if the meta field
    // is part of the shard key.
    const middle = shardKeyHasMetaField ? {meta: 1} : {"meta.a": 1};
    assert.commandWorked(
        mongos0.adminCommand({split: `${dbName}.${getTimeseriesCollForDDLOps(mongos0, collName)}`, middle}),
    );

    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(
        mongos0.adminCommand({
            movechunk: `${dbName}.${getTimeseriesCollForDDLOps(mongos0, collName)}`,
            find: middle,
            to: otherShard.name,
            _waitForDelete: true,
        }),
    );

    assert.commandWorked(mongos1.runCommand(cmdObj));

    // Insert dummy data so that the 'mongos1' sees the collection as sharded.
    assert.commandWorked(mongos1.getCollection(collName).insert({[timeField]: ISODate()}));

    // Drop and recreate an unsharded collection with 'mongos0' as the router.
    assert(mongos0.getCollection(collName).drop());
    assert.commandWorked(
        mongos0.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}),
    );

    assert.commandWorked(mongos1.runCommand(cmdObj));
}

/**
 * Commands on the timeseries measurements.
 */
runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        createIndexes: collName,
        indexes: [{key: {[timeField]: 1}, name: "index_on_time"}],
    },
});

runTest({shardKey: {[metaField]: 1}, cmdObj: {listIndexes: collName}});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        dropIndexes: collName,
        index: "*",
    },
});

runTest({shardKey: {[metaField]: 1}, cmdObj: {collMod: collName, expireAfterSeconds: 3600}});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {insert: collName, documents: [{[timeField]: ISODate()}]},
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {insert: collName, documents: [{[timeField]: ISODate()}]},
});

runTest({shardKey: {[metaField]: 1}, cmdObj: {aggregate: collName, pipeline: [], cursor: {}}});

runTest({shardKey: {[metaField]: 1}, cmdObj: {collStats: collName}});

/**
 * Commands on the raw buckets
 */
runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        createIndexes: getTimeseriesCollForRawOps(mongos0, collName),
        indexes: [{key: {[timeField]: 1}, name: "index_on_time"}],
        ...getRawOperationSpec(mongos0),
    },
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        listIndexes: getTimeseriesCollForRawOps(mongos0, collName),
        ...getRawOperationSpec(mongos0),
    },
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        dropIndexes: getTimeseriesCollForRawOps(mongos0, collName),
        index: "*",
        ...getRawOperationSpec(mongos0),
    },
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        aggregate: getTimeseriesCollForRawOps(mongos0, collName),
        pipeline: [],
        cursor: {},
        ...getRawOperationSpec(mongos0),
    },
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        insert: getTimeseriesCollForRawOps(mongos0, collName),
        documents: [
            {
                _id: ObjectId(),
                control: {
                    min: {time: testTimestamp},
                    max: {time: testTimestamp},
                    version: TimeseriesTest.BucketVersion.kUncompressed,
                },
                data: {[timeField]: {0: testTimestamp}},
            },
        ],
        ...getRawOperationSpec(mongos0),
    },
});

// Tests for updates.
runTest({
    shardKey: {[metaField + ".a"]: 1},
    cmdObj: {
        update: collName,
        updates: [
            {
                q: {[metaField + ".a"]: 1},
                u: {$inc: {[metaField + ".b"]: -1}},
                multi: true,
            },
        ],
    },
});

runTest({
    shardKey: {[metaField + ".a"]: 1},
    cmdObj: {
        update: getTimeseriesCollForRawOps(mongos0, collName),
        updates: [
            {
                q: {["meta.a"]: 1},
                u: {$inc: {["meta.b"]: -1}},
                multi: true,
            },
        ],
        ...getRawOperationSpec(mongos0),
    },
});

// Tests for deletes.
runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        delete: collName,
        deletes: [
            {
                q: {[metaField]: 0},
                limit: 0,
            },
        ],
    },
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        delete: getTimeseriesCollForRawOps(mongos0, collName),
        deletes: [
            {
                q: {meta: 0},
                limit: 0,
            },
        ],
        ...getRawOperationSpec(mongos0),
    },
});

st.stop();
