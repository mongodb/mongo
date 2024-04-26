/**
 * Test various commands on time-series collection in the presence of multiple mongos and collection
 * changing from unsharded to sharded and vice versa.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

Random.setRandomSeed();

const dbName = 'testDB';
const collName = 'testColl';
const bucketsCollName = 'system.buckets.' + collName;
const timeField = 'time';
const metaField = 'hostid';
const testTimestamp = ISODate();

// Connections.
const st = new ShardingTest({mongos: 2, shards: 2, rs: {nodes: 2}});
const mongos0 = st.s0.getDB(dbName);
const mongos1 = st.s1.getDB(dbName);
const shard0DB = st.shard0.getDB(dbName);
const shard1DB = st.shard1.getDB(dbName);

// Databases and collections.
assert.commandWorked(mongos0.adminCommand({enableSharding: dbName}));

// Helpers.
let currentId = 0;
function generateId() {
    return currentId++;
}

// TODO SERVER-87797 remove completely the unsharded field. The test starts by creating an
// unsharded collection with mongo1 and sharding it with mongo0. The command specified in the test
// case is then run with mongos1 (stale). The sharded collection is then dropped and re-created as
// unsharded from mongos0 and re-attempted the command from mongos1. The test searches in
// system.profile that the command run 2 times (1 per shard) for the sharded case and 1 time for the
// unsharded case. The test is testing specifically the interaction within the sharding protocol
// which is way beyond the scope of the test. Rewrite this test to check the command simply works on
// the stale mongos
const isTrackUnshardedEnabled = FeatureFlagUtil.isPresentAndEnabled(
                                    st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation") ||
    FeatureFlagUtil.isPresentAndEnabled(st.s.getDB('admin'),
                                        "TrackUnshardedCollectionsUponMoveCollection");

/**
 * The test runs drop, create and shardCollection commands using mongos0, then validates the given
 * command by running against mongos1 with stale config.
 */
function runTest({shardKey, cmdObj, numProfilerEntries}) {
    if (isTrackUnshardedEnabled) {
        return;
    }

    const isInsert = cmdObj["insert"] !== undefined;
    const isDelete = cmdObj["delete"] !== undefined;
    const isUpdate = cmdObj["update"] !== undefined;
    const isCollMod = cmdObj["collMod"] !== undefined;
    const isDropIndex = cmdObj["dropIndexes"] !== undefined;
    const cmdCollName = cmdObj[Object.keys(cmdObj)[0]];
    const shardKeyHasMetaField = shardKey[metaField] !== undefined;

    // Insert some dummy data using 'mongos1' as the router, so that the cache is initialized on the
    // mongos while the collection is unsharded.
    assert.commandWorked(mongos1.getCollection(collName).insert({[timeField]: ISODate()}));

    // Drop and shard the collection with 'mongos0' as the router.
    assert(mongos0.getCollection(collName).drop());
    assert.commandWorked(mongos0.createCollection(
        collName, {timeseries: {timeField: timeField, metaField: metaField}}));
    assert.commandWorked(st.s0.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: shardKey,
    }));

    // Move one of the chunks into the second shard. Note that we can only do this if the meta field
    // is part of the shard key.
    const middle = shardKeyHasMetaField ? {meta: 1} : {"meta.a": 1};
    assert.commandWorked(mongos0.adminCommand({split: `${dbName}.${bucketsCollName}`, middle}));

    const primaryShard = st.getPrimaryShard(dbName);
    const otherShard = st.getOther(primaryShard);
    assert.commandWorked(mongos0.adminCommand({
        movechunk: `${dbName}.${bucketsCollName}`,
        find: middle,
        to: otherShard.name,
        _waitForDelete: true
    }));

    // Validate the command by running against 'mongos1' as the router.
    function validateCommand(cmdCollName, numEntries, unVersioned) {
        // Restart profiler.
        for (let shardDB of [shard0DB, shard1DB]) {
            shardDB.setProfilingLevel(0);
            shardDB.system.profile.drop();
            shardDB.setProfilingLevel(2);
        }

        const res = mongos1.runCommand(cmdObj);
        if (numEntries > 0) {
            assert.commandWorked(res);
        } else {
            assert.commandFailed(res);
        }

        const queryField = `command.${Object.keys(cmdObj)[0]}`;
        let filter = {
            [queryField]: cmdCollName,
            "$or": [
                {
                    "$and": [
                        {"command.shardVersion.0": {"$exists": true}},
                        {
                            "command.shardVersion.0":
                                {$ne: ShardVersioningUtil.kIgnoredShardVersion.v}
                        },
                    ]
                },
                {
                    "$and": [
                        {"command.shardVersion.v": {"$exists": true}},
                        {
                            "command.shardVersion.v":
                                {$ne: ShardVersioningUtil.kIgnoredShardVersion.v}
                        },
                    ]
                },
            ]
        };

        if (isUpdate) {
            // We currently do not log 'shardVersion' for updates. See SERVER-60354 for details.
            filter = {
                $or: [
                    {op: 'update'},
                    {op: 'bulkWrite', "command.update": {$exists: true}},
                ],
                "ns": `${dbName}.${cmdCollName}`,
                "ok": {$ne: 0}
            };
        } else if (isDelete) {
            filter = {
                $or: [
                    {op: 'remove'},
                    {op: 'bulkWrite', "command.delete": {$exists: true}},
                ],
                "ns": `${dbName}.${cmdCollName}`,
                "ok": {$ne: 0}
            };
        } else if (isCollMod) {
            const command = unVersioned ? "_shardsvrCollMod" : "_shardsvrCollModParticipant";
            filter = {[`command.${command}`]: cmdCollName, "ok": {$ne: 0}};
        } else if (unVersioned && !isCollMod) {
            filter = {
                [queryField]: cmdCollName,
                "$or": [
                    {"command.shardVersion.0": ShardVersioningUtil.kIgnoredShardVersion.v},
                    {"command.shardVersion.v": ShardVersioningUtil.kIgnoredShardVersion.v},
                ]
            };
        }

        if (isInsert) {
            filter = {
                $or: [
                    filter,
                    {op: 'bulkWrite', "command.insert": {$exists: true}},
                ]
            }
        }

        // Filter out the profiler entries with $indexStats pipeline stage, as the
        // PeriodicShardedIndexConsistencyChecker can issue an aggregate command with
        // '[$indexStats: {}]' pipeline periodically. We don't want these entries to be accounted
        // here.
        filter["command.pipeline.0.$indexStats"] = {$exists: false};

        const shard0Entries = shard0DB.system.profile.find(filter).toArray();
        const shard1Entries = shard1DB.system.profile.find(filter).toArray();
        assert.eq(shard0Entries.length + shard1Entries.length,
                  numEntries,
                  {shard0Entries: shard0Entries, shard1Entries: shard1Entries});
    }

    // The update command is always logged as being on the user-provided namespace.
    validateCommand((isUpdate || isDelete || isCollMod) ? cmdCollName : bucketsCollName,
                    numProfilerEntries.sharded);

    // Insert dummy data so that the 'mongos1' sees the collection as sharded.
    assert.commandWorked(mongos1.getCollection(collName).insert({[timeField]: ISODate()}));

    // Drop and recreate an unsharded collection with 'mongos0' as the router.
    assert(mongos0.getCollection(collName).drop());
    assert.commandWorked(mongos0.createCollection(
        collName, {timeseries: {timeField: timeField, metaField: metaField}}));

    // When unsharded, the command should be run against the user requested namespace, except for
    // dropIndex.
    validateCommand(
        isDropIndex ? bucketsCollName : cmdCollName, numProfilerEntries.unsharded, true);
}

/**
 * Commands on the view namespace.
 */
runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        createIndexes: collName,
        indexes: [{key: {[timeField]: 1}, name: "index_on_time"}],
    },
    numProfilerEntries: {sharded: 2, unsharded: 1}
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {listIndexes: collName},
    numProfilerEntries: {sharded: 1, unsharded: 1}
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        dropIndexes: collName,
        index: "*",
    },
    numProfilerEntries: {sharded: 2, unsharded: 1}
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {collMod: collName, expireAfterSeconds: 3600},
    numProfilerEntries: {sharded: 2, unsharded: 1}
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {insert: collName, documents: [{[timeField]: ISODate()}]},
    numProfilerEntries: {sharded: 1, unsharded: 1}
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {insert: collName, documents: [{[timeField]: ISODate()}]},
    numProfilerEntries: {sharded: 1, unsharded: 1}
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {aggregate: collName, pipeline: [], cursor: {}},
    numProfilerEntries: {sharded: 2, unsharded: 1}
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {collStats: collName},
    numProfilerEntries: {sharded: 2, unsharded: 1}
});

/**
 * On system.buckets namespace
 */
runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        createIndexes: bucketsCollName,
        indexes: [{key: {[timeField]: 1}, name: "index_on_time"}],
    },
    numProfilerEntries: {sharded: 2, unsharded: 1},
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {listIndexes: bucketsCollName},
    numProfilerEntries: {sharded: 1, unsharded: 1},
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        dropIndexes: bucketsCollName,
        index: "*",
    },
    numProfilerEntries: {sharded: 2, unsharded: 1},
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {collMod: bucketsCollName, expireAfterSeconds: 3600},
    numProfilerEntries: {sharded: 2, unsharded: 1},
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {aggregate: bucketsCollName, pipeline: [], cursor: {}},
    numProfilerEntries: {sharded: 2, unsharded: 1},
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        insert: bucketsCollName,
        documents: [{
            _id: ObjectId(),
            control: {
                min: {time: testTimestamp},
                max: {time: testTimestamp},
                version: TimeseriesTest.BucketVersion.kUncompressed
            },
            data: {[timeField]: {0: testTimestamp}},
        }]
    },
    numProfilerEntries: {sharded: 1, unsharded: 1},
});

// Tests for updates.
runTest({
    shardKey: {[metaField + ".a"]: 1},
    cmdObj: {
        update: collName,
        updates: [{
            q: {[metaField + ".a"]: 1},
            u: {$inc: {[metaField + ".b"]: -1}},
            multi: true,
        }]
    },
    numProfilerEntries: {sharded: 1, unsharded: 1},
});

runTest({
    shardKey: {[metaField + ".a"]: 1},
    cmdObj: {
        update: bucketsCollName,
        updates: [{
            q: {["meta.a"]: 1},
            u: {$inc: {["meta.b"]: -1}},
            multi: true,
        }]
    },
    numProfilerEntries: {sharded: 1, unsharded: 1},
});

// Tests for deletes.
runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        delete: collName,
        deletes: [{
            q: {[metaField]: 0},
            limit: 0,
        }],
    },
    numProfilerEntries: {sharded: 1, unsharded: 1},
});

runTest({
    shardKey: {[metaField]: 1},
    cmdObj: {
        delete: bucketsCollName,
        deletes: [{
            q: {meta: 0},
            limit: 0,
        }],
    },
    numProfilerEntries: {sharded: 1, unsharded: 1},
});

st.stop();
