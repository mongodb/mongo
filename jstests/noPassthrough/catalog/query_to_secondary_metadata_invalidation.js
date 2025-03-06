/**
 * Tests that the query to the secondary is killed if a range deletion was executed during the
 * query execution.
 *
 * @tags: [
 *   featureFlagTerminateSecondaryReadsUponRangeDeletion
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {nodes: 2},
    other: {
        enableBalancer: false,
        rsOptions: {
            setParameter: {orphanCleanupDelaySecs: 0, terminateSecondaryReadsOnOrphanCleanup: true}
        }
    }
});

const dbName = "test";
const collName = "coll";
const collFullName = "test.coll";
const db = st.s.getDB(dbName);
const coll = db.getCollection(collName);

function getFindCommand(readConcernLevel = "local") {
    const findCommand = {
        find: collName,
        $readPreference: {mode: "secondary"},
        readConcern: {level: readConcernLevel},
        filter: {},
        batchSize: 5
    };
    return findCommand;
}

function moveRange(ns, targetShard) {
    assert.commandWorked(
        st.s.adminCommand({moveRange: ns, min: {x: 10}, max: {x: 100}, toShard: targetShard}));
}

function waitUntilMoveRangeFinished(shard) {
    assert.soon(() => {
        return shard.getDB("config").getCollection("rangeDeletions").find().toArray().length == 0;
    });
}

// create database and make shard0 db primary shard
assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

// create sharded coll
CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: 100}, shard: st.shard0.shardName},
    {min: {x: 100}, max: {x: MaxKey}, shard: st.shard1.shardName}
]);

let insertions = [];
for (let i = 0; i < 100; ++i) {
    insertions.push({x: i});
}
coll.insertMany(insertions);

function runFindCommandWithCursor(readConcernLevel,
                                  mustFail,
                                  donorShard,
                                  targetShard,
                                  expectedItemCounter = 100,
                                  removeShardVersion = false) {
    const cursor = new DBCommandCursor(
        db, assert.commandWorked(db.runCommand(getFindCommand(readConcernLevel))));

    if (removeShardVersion) {
        let hangBeforeSendingCommitDecisionFP =
            configureFailPoint(donorShard, "hangBeforeSendingCommitDecision");

        const awaitResult = startParallelShell(
            funWithArgs(function(ns, toShardName) {
                assert.commandWorked(db.adminCommand(
                    {moveRange: ns, min: {x: 10}, max: {x: 100}, toShard: toShardName}));
            }, collFullName, targetShard.shardName), st.s.port);

        hangBeforeSendingCommitDecisionFP.wait();

        assert.eq(
            1, donorShard.getDB("config").getCollection("rangeDeletions").find().toArray().length);
        assert.eq(1,
                  donorShard.getDB("config")
                      .getCollection("rangeDeletions")
                      .find({"preMigrationShardVersion": {$exists: true}})
                      .toArray()
                      .length);
        donorShard.getDB("config")
            .getCollection("rangeDeletions")
            .updateOne({"preMigrationShardVersion": {$exists: true}},
                       {$unset: {"preMigrationShardVersion": ""}});
        assert.eq(0,
                  donorShard.getDB("config")
                      .getCollection("rangeDeletions")
                      .find({"preMigrationShardVersion": {$exists: true}})
                      .toArray()
                      .length);

        hangBeforeSendingCommitDecisionFP.off();

        awaitResult();
    } else {
        moveRange(collFullName, targetShard.shardName);
    }

    waitUntilMoveRangeFinished(donorShard);

    if (mustFail) {
        assert.throwsWithCode(() => cursor.itcount(), ErrorCodes.QueryPlanKilled);
    } else {
        assert.eq(expectedItemCounter,
                  cursor.itcount(),
                  "failed to read the expected documents from the secondary");
    }
}

// The query must be killed on secondary.
runFindCommandWithCursor('local', true, st.shard0, st.shard1);
// Test when the range is moved back.
runFindCommandWithCursor('local', true, st.shard1, st.shard0);

// The query should not be killed for 'snapshot' and 'available' read concerns.
runFindCommandWithCursor('snapshot', false, st.shard0, st.shard1);
// With 'available' the query returns the wrong result.
runFindCommandWithCursor('available', false, st.shard1, st.shard0, 105);

// The query should not be killed when a rangeDeletion document is processed and it doesn’t contain
// the preMigrationShardVersion field, but it returns the wrong result.
runFindCommandWithCursor('local', false, st.shard0, st.shard1, 10, true);

// Disable terminateSecondaryReadsOnOrphanCleanup
const shards = st.getAllShards();
shards.forEach((rs) => {
    rs.getSecondaries().forEach((conn) => {
        assert.commandWorked(
            conn.adminCommand({setParameter: 1, terminateSecondaryReadsOnOrphanCleanup: false}));
    });
});

// The query should not be killed when the server parameter terminateSecondaryReadsOnOrphanCleanup
// is disabled, but it returns the wrong result.
runFindCommandWithCursor('local', false, st.shard1, st.shard0, 15);

// Enable terminateSecondaryReadsOnOrphanCleanup
shards.forEach((rs) => {
    rs.getSecondaries().forEach((conn) => {
        assert.commandWorked(
            conn.adminCommand({setParameter: 1, terminateSecondaryReadsOnOrphanCleanup: true}));
    });
});

// Test that queries initiated after the start of a range deletion are not killed.
const cursorBefore = new DBCommandCursor(db, assert.commandWorked(db.runCommand(getFindCommand())));

moveRange(collFullName, st.shard1.shardName);

const cursorAfter = new DBCommandCursor(db, assert.commandWorked(db.runCommand(getFindCommand())));
waitUntilMoveRangeFinished(st.shard0);

// The query initiated before a range deletion begins must be terminated.
assert.throwsWithCode(() => cursorBefore.itcount(), ErrorCodes.QueryPlanKilled);
// The query initiated after a range deletion begins must not be killed.
assert.eq(100, cursorAfter.itcount(), "failed to read the documents from the secondary");

st.stop();
