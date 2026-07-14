/**
 * Tests that renameCollection on a sharded cluster fails immediately with
 * BackgroundOperationInProgressForNamespace when an index build is in progress
 * on the source or target collection.
 *
 * Regression test for SERVER-117624.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

/**
 * Creates a sharded collection with chunks on both shards.
 */
function createShardedCollection(dbName, collName) {
    const db = st.s.getDB(dbName);
    const coll = db.getCollection(collName);

    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );
    assert.commandWorked(
        coll.insert([
            {_id: 0, x: -1},
            {_id: 1, x: 1},
        ]),
    );
    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {x: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: coll.getFullName(), find: {x: 1}, to: st.shard1.shardName}),
    );

    return coll;
}

/**
 * Creates an unsharded collection via insert through mongos.
 */
function createUntrackedCollection(dbName, collName) {
    const db = st.s.getDB(dbName);
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );
    const coll = db.getCollection(collName);
    assert.commandWorked(coll.insert({_id: 0, x: -1}));
    return coll;
}

/**
 * Runs renameCollection in a parallel shell and asserts it fails with
 * BackgroundOperationInProgressForNamespace.
 */
function assertRenameFailsImmediately(sourceNs, targetNs, dropTarget) {
    const awaitRename = startParallelShell(
        funWithArgs(
            function (sourceNs, targetNs, dropTarget) {
                const cmd = {renameCollection: sourceNs, to: targetNs};
                if (dropTarget) {
                    cmd.dropTarget = true;
                }
                const res = db.getSiblingDB("admin").runCommand(cmd);
                assert.commandFailedWithCode(
                    res,
                    ErrorCodes.BackgroundOperationInProgressForNamespace,
                    "rename should fail immediately when an index build is in progress",
                );
            },
            sourceNs,
            targetNs,
            dropTarget,
        ),
        st.s.port,
    );
    awaitRename();
}

/**
 * Hangs an index build on shard0 for the given collection and returns the
 * failpoint and the await function.
 */
function hangIndexBuildOnShard0(coll, indexKey) {
    const shard0Primary = st.shard0.rs.getPrimary();
    const shard0DB = shard0Primary.getDB(coll.getDB().getName());
    const hangFp = configureFailPoint(shard0Primary, "hangIndexBuildBeforeCommit");
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(st.s, coll.getFullName(), indexKey);
    const indexName = Object.keys(indexKey)
        .map((k) => k + "_1")
        .join("_");
    IndexBuildTest.waitForIndexBuildToStart(shard0DB, coll.getName(), indexName);
    return {hangFp, awaitIndexBuild};
}

// ---------------------------------------------------------------------------
// Test 1: Index build on a sharded source collection.
// ---------------------------------------------------------------------------
(function testIndexBuildOnShardedSource() {
    const dbName = jsTestName() + "_tsrc";
    const collName = "source";
    const targetCollName = "target";
    const coll = createShardedCollection(dbName, collName);

    const {hangFp, awaitIndexBuild} = hangIndexBuildOnShard0(coll, {y: 1});

    assertRenameFailsImmediately(coll.getFullName(), dbName + "." + targetCollName);
    jsTest.log("Test 1: rename failed immediately due to index build on sharded source");

    assert.commandWorked(coll.insert({_id: 100, x: -2}));

    hangFp.off();
    awaitIndexBuild();
    assert.commandWorked(
        st.s.adminCommand({
            renameCollection: coll.getFullName(),
            to: dbName + "." + targetCollName,
        }),
    );
    jsTest.log("Test 1: rename succeeded after index build completed");
})();

// ---------------------------------------------------------------------------
// Test 2: Index build on an unsharded source collection.
// ---------------------------------------------------------------------------
(function testIndexBuildOnUnshardedSource() {
    const dbName = jsTestName() + "_usrc";
    const collName = "source";
    const targetCollName = "target";
    const coll = createUntrackedCollection(dbName, collName);

    const {hangFp, awaitIndexBuild} = hangIndexBuildOnShard0(coll, {y: 1});

    assertRenameFailsImmediately(coll.getFullName(), dbName + "." + targetCollName);
    jsTest.log("Test 2: rename failed immediately due to index build on unsharded source");

    assert.commandWorked(coll.insert({_id: 101, x: -2}));

    hangFp.off();
    awaitIndexBuild();
    assert.commandWorked(
        st.s.adminCommand({
            renameCollection: coll.getFullName(),
            to: dbName + "." + targetCollName,
        }),
    );
    jsTest.log("Test 2: rename succeeded after index build completed");
})();

// ---------------------------------------------------------------------------
// Test 3: Index build on an unsharded target collection (dropTarget).
// ---------------------------------------------------------------------------
(function testIndexBuildOnUnshardedTarget() {
    const dbName = jsTestName() + "_utgt";
    const collName = "source";
    const targetCollName = "target";
    const coll = createShardedCollection(dbName, collName);

    const targetColl = st.s.getDB(dbName).getCollection(targetCollName);
    assert.commandWorked(targetColl.insert({_id: 0, a: 1}));

    const {hangFp, awaitIndexBuild} = hangIndexBuildOnShard0(targetColl, {b: 1});

    assertRenameFailsImmediately(coll.getFullName(), dbName + "." + targetCollName, true);
    jsTest.log("Test 3: rename failed immediately due to index build on unsharded target");

    assert.commandWorked(coll.insert({_id: 100, x: -2}));
    assert.commandWorked(targetColl.insert({_id: 200, a: 2}));

    hangFp.off();
    awaitIndexBuild();
    assert.commandWorked(
        st.s.adminCommand({
            renameCollection: coll.getFullName(),
            to: dbName + "." + targetCollName,
            dropTarget: true,
        }),
    );
    jsTest.log("Test 3: rename succeeded after index build completed");
})();

// ---------------------------------------------------------------------------
// Test 4: Index build on a sharded target collection (dropTarget).
// ---------------------------------------------------------------------------
(function testIndexBuildOnShardedTarget() {
    const dbName = jsTestName() + "_ttgt";
    const collName = "source";
    const targetCollName = "target";
    const coll = createShardedCollection(dbName, collName);

    const targetColl = createShardedCollection(dbName, targetCollName);

    const {hangFp, awaitIndexBuild} = hangIndexBuildOnShard0(targetColl, {b: 1});

    assertRenameFailsImmediately(coll.getFullName(), dbName + "." + targetCollName, true);
    jsTest.log("Test 4: rename failed immediately due to index build on sharded target");

    assert.commandWorked(coll.insert({_id: 100, x: -2}));
    assert.commandWorked(targetColl.insert({_id: 100, x: -2}));

    hangFp.off();
    awaitIndexBuild();
    assert.commandWorked(
        st.s.adminCommand({
            renameCollection: coll.getFullName(),
            to: dbName + "." + targetCollName,
            dropTarget: true,
        }),
    );
    jsTest.log("Test 4: rename succeeded after index build completed");
})();

st.stop();
