/**
 * This test is meant to test that a migration will correctly wait for the majority commit point
 * when there are no transfer mod writes (SERVER-42783).
 * @tags: [
 *   requires_majority_read_concern,
 * ]
 */

import {
    migrateStepNames,
    moveChunkParallel,
    pauseMigrateAtStep,
    unpauseMigrateAtStep,
    waitForMigrateStep,
} from "jstests/libs/chunk_manipulation_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// Set up a sharded cluster with two shards, two chunks, and one document in one of the chunks.
const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const kDbName = "test";

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));
const testDB = st.s.getDB(kDbName);
assert.commandWorked(testDB.foo.insert({_id: 1}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({shardCollection: "test.foo", key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: "test.foo", middle: {_id: 0}}));
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(
    st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

// The document is in the majority committed snapshot.
assert.eq(1, testDB.foo.find().readConcern("majority").itcount());

// Advance a migration to the beginning of the cloning phase.
pauseMigrateAtStep(st.rs1.getPrimary(), migrateStepNames.rangeDeletionTaskScheduled);

// For startParallelOps to write its state
let staticMongod = MongoRunner.runMongod({});

let awaitMigration = moveChunkParallel(
    staticMongod,
    st.s.host,
    {_id: 1},
    null,
    "test.foo",
    st.shard1.shardName,
    false /* expectSuccess */,
);

// Wait for the migration to reach the failpoint and allow any writes to become majority committed
// before pausing replication.
waitForMigrateStep(st.rs1.getPrimary(), migrateStepNames.rangeDeletionTaskScheduled);
st.rs1.awaitLastOpCommitted();

// Disable replication on the recipient shard's secondary node, so the recipient shard's majority
// commit point cannot advance.
const destinationSec = st.rs1.getSecondary();
stopServerReplication(destinationSec);

// Allow the migration to begin cloning.
unpauseMigrateAtStep(st.rs1.getPrimary(), migrateStepNames.rangeDeletionTaskScheduled);

// Check the migration coordinator document, because the moveChunk command itself
// will hang on trying to remove the recipient's range deletion entry with majority writeConcern
// until replication is re-enabled on the recipient.
assert.soon(() => {
    return (
        st.rs0.getPrimary().getDB("config").getCollection("migrationCoordinators").findOne({
            nss: "test.foo",
            "range.min._id": 0,
            "range.max._id": MaxKey,
            decision: "aborted",
        }) != null
    );
});

restartServerReplication(destinationSec);

awaitMigration();

st.stop();
MongoRunner.stopMongod(staticMongod);
