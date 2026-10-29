/**
 * A find that triggers authoritative disk recovery times out on maxTimeMS while recovery is blocked;
 * recovery then finishes in the background without any other client keeping the refresh alive.
 *
 * @tags: [requires_fcv_90, requires_persistence]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Disable index consistency checks so the config server does not trigger StaleShardVersion on
// shards and cause extra metadata refreshes that race with disk recovery.
const nodeOptions = {setParameter: {enableShardedIndexConsistencyCheck: false}};

function getDiskRecoveryCount(shard) {
    return Number(
        shard.getDB("admin").serverStatus().shardingStatistics.collectionShardingMetadataStatistics
            .countDiskRecoveriesPerformed,
    );
}

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    rs: {nodes: 1},
    other: {configOptions: nodeOptions, shardOptions: nodeOptions},
});
const dbName = jsTestName();
const collName = "coll";
const ns = `${dbName}.${collName}`;
const coll = st.s.getDB(dbName).getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.shardCollection(ns, {key: 1});
assert.commandWorked(coll.insert({key: -1}));

st.restartShardRS(0, true /* waitForPrimary */);

const hangRecoveryFp = configureFailPoint(st.shard0, "hangInRecoverRefreshThread");
const recoveriesBefore = getDiskRecoveryCount(st.shard0);
const mongosColl = st.s.getDB(dbName).getCollection(collName);

function findTimesOutWhileRecoveryHung(dbNameArg, collNameArg) {
    const err = assert.throws(() =>
        db
            .getSiblingDB(dbNameArg)
            .getCollection(collNameArg)
            .find({key: -1})
            .maxTimeMS(500)
            .itcount(),
    );
    assert.contains(err.code, [ErrorCodes.MaxTimeMSExpired, ErrorCodes.ExceededTimeLimit]);
}

const shell = startParallelShell(
    funWithArgs(findTimesOutWhileRecoveryHung, dbName, collName),
    st.s.port,
);

assert(hangRecoveryFp.wait());

shell();

hangRecoveryFp.off();

assert.soon(() => getDiskRecoveryCount(st.shard0) === recoveriesBefore + 1);

// The metadata is now installed, so a subsequent find must be served from the cached metadata
// without triggering any further disk recovery on the shard.
const recoveriesAfterInstall = getDiskRecoveryCount(st.shard0);
assert.eq(mongosColl.find({key: -1}).itcount(), 1);
assert.eq(getDiskRecoveryCount(st.shard0), recoveriesAfterInstall);

st.stop();
