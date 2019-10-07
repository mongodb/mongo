/**
 * This test is meant to test that a migration will correctly wait for the majority commit point
 * when there are no transfer mod writes (SERVER-42783).
 * @tags: [requires_find_command]
 */

(function() {
    "use strict";

    load('./jstests/libs/chunk_manipulation_util.js');

    // Set up a sharded cluster with two shards, two chunks, and one document in one of the chunks.
    const st = new ShardingTest({shards: 2, rs: {nodes: 2}, config: 1});
    const testDB = st.s.getDB("test");

    assert.commandWorked(testDB.foo.insert({_id: 1}, {writeConcern: {w: "majority"}}));

    st.ensurePrimaryShard("test", st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
    assert.commandWorked(st.s.adminCommand({shardCollection: "test.foo", key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: "test.foo", middle: {_id: 0}}));

    // The document is in the majority committed snapshot.
    assert.eq(1, testDB.foo.find().readConcern("majority").itcount());

    // Advance a migration to the beginning of the cloning phase.
    pauseMigrateAtStep(st.rs1.getPrimary(), 2);

    // For startParallelOps to write its state
    let staticMongod = MongoRunner.runMongod({});

    let awaitMigration = moveChunkParallel(staticMongod,
                                           st.s.host,
                                           {_id: 1},
                                           null,
                                           "test.foo",
                                           st.shard1.shardName,
                                           false /* expectSuccess */);

    // Wait for the migration to reach the failpoint and allow any writes to become majority
    // committed
    // before pausing replication.
    waitForMigrateStep(st.rs1.getPrimary(), 2);
    st.rs1.awaitLastOpCommitted();

    // Disable replication on the recipient shard's secondary node, so the recipient shard's
    // majority
    // commit point cannot advance.
    const destinationSec = st.rs1.getSecondary();
    assert.commandWorked(
        destinationSec.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "alwaysOn"}),
        "failed to enable fail point on secondary");

    // Allow the migration to begin cloning.
    unpauseMigrateAtStep(st.rs1.getPrimary(), 2);

    // The migration should fail to commit without being able to advance the majority commit point.
    awaitMigration();

    assert.commandWorked(
        destinationSec.adminCommand({configureFailPoint: "rsSyncApplyStop", mode: "off"}),
        "failed to enable fail point on secondary");

    st.stop();
    MongoRunner.stopMongod(staticMongod);
})();