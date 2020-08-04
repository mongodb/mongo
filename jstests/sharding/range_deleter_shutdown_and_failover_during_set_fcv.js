/**
 * Tests that if a shard is shut down or stepped down while performing the range-deleter related
 * parts of setFCV to 4.4, the config server can run setFCV against the new shard primary and the
 * setFCV can still complete normally.
 *
 * requires_persistence - This test restarts shards and expects them to remember their data.
 * requires_fcv_44 - This test sets the FCV to 4.4.
 * @tags: [requires_persistence, requires_fcv_44]
 */

(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

function setUp() {
    // Set FCV to 4.2 since we want to test upgrading the FCV to 4.4.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.2"}));

    // Create a database and enable sharding on it (enableSharding implicitly creates the database).
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    // Shard a collection with some initial chunks on each shard.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: "hashed"}}));
    assert.gt(st.s.getDB("config").chunks.count({shard: st.shard0.shardName, ns: ns}), 0);
    assert.gt(st.s.getDB("config").chunks.count({shard: st.shard1.shardName, ns: ns}), 0);
}

function tearDown() {
    assert.commandWorked(st.s.getDB(dbName).dropDatabase());
}

const dbName = "db1";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({
    shards: 2,
    config: 1,
    other: {
        rs: true,
        rsOptions: {nodes: 3},
    }
});

(() => {
    jsTest.log("Test *shutting* down a shard primary while it is *enumerating orphaned ranges*");
    setUp();
    const originalRS0Primary = st.rs0.getPrimary();

    let failpoint =
        configureFailPoint(originalRS0Primary, "setFCVHangWhileEnumeratingOrphanedRanges");
    const awaitResult = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
    }, st.s.port);
    failpoint.wait();

    // Kill replica set primary with signal 9.
    st.rs0.stop(
        originalRS0Primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

    // The setFCV can still succeed.
    awaitResult();

    // Reset the cluster state. No need to turn the failpoint off since the node was restarted.
    st.rs0.start(originalRS0Primary, {waitForConnect: true}, true /* restart */);
    tearDown();
})();

(() => {
    jsTest.log("Test *stepping* down a shard primary while it is *enumerating orphaned ranges*");
    setUp();
    const originalRS0Primary = st.rs0.getPrimary();

    let failpoint =
        configureFailPoint(originalRS0Primary, "setFCVHangWhileEnumeratingOrphanedRanges");
    const awaitResult = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
    }, st.s.port);
    failpoint.wait();

    assert.commandWorked(originalRS0Primary.adminCommand({replSetStepDown: 60, force: true}));

    // The setFCV can still succeed.
    awaitResult();

    // Reset the cluster state.
    failpoint.off();
    tearDown();
})();

(() => {
    jsTest.log("Test *shutting* down a shard primary while it is *inserting range deletion tasks*");
    setUp();
    const originalRS0Primary = st.rs0.getPrimary();

    let failpoint =
        configureFailPoint(originalRS0Primary, "setFCVHangWhileInsertingRangeDeletionTasks");
    const awaitResult = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
    }, st.s.port);
    failpoint.wait();

    // Kill replica set primary with signal 9.
    st.rs0.stop(
        originalRS0Primary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

    // The setFCV can still succeed.
    awaitResult();

    // Reset the cluster state. No need to turn the failpoint off since the node was restarted.
    st.rs0.start(originalRS0Primary, {waitForConnect: true}, true /* restart */);
    tearDown();
})();

(() => {
    jsTest.log("Test *stepping* down a shard primary while it is *inserting range deletion tasks*");
    setUp();
    const originalRS0Primary = st.rs0.getPrimary();

    let failpoint =
        configureFailPoint(originalRS0Primary, "setFCVHangWhileInsertingRangeDeletionTasks");
    const awaitResult = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
    }, st.s.port);
    failpoint.wait();

    assert.commandWorked(originalRS0Primary.adminCommand({replSetStepDown: 60, force: true}));

    // The setFCV can still succeed.
    awaitResult();

    // Reset the cluster state.
    failpoint.off();
    tearDown();
})();

st.stop();
})();
