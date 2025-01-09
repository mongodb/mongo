/*
 * Tests that the checkMetadataConsistency command can be interrupted at important points,
 * and that this interruption propagates to its subcommands.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    assertCommandFailedWithCodeInParallelShell,
} from "jstests/libs/parallel_shell_helpers.js";

/**
 * Finds the opid of the (unique) command matching a filter over `$currentOp`.
 */
function tryFindOpid(conn, cmdFilter) {
    const matchingOps = conn.getDB("admin")
                            .aggregate([{$currentOp: {localOps: true}}, {$match: cmdFilter}])
                            .toArray();
    assert(matchingOps.length <= 1,
           "Ambiguous match for command matching " + tojsononeline(cmdFilter) +
               ", found: " + tojson(matchingOps));
    return matchingOps.length === 1 && matchingOps[0].opid != null ? matchingOps[0].opid : null;
}

function findOpid(conn, cmdFilter) {
    const opid = tryFindOpid(conn, cmdFilter);
    assert(opid != null, "Failed to find command matching " + tojsononeline(cmdFilter));
    return opid;
}

/**
 * Runs a command and checks that when it is killed while hung at fail point `hangFailPointName`,
 * it gets interrupted before it reaches fail point `deadlineFailPointName`.
 */
function assertCommandInterruptsBetweenFailPoints(
    conn, {dbName, command}, hangFailPointName, deadlineFailPointName) {
    jsTestLog("Checking that " + tojsononeline(command) + " over db=" + dbName +
              " is interruptable between " + hangFailPointName + " and " + deadlineFailPointName);

    // Configure the fail points and launch the command.
    const hangFailPoint = configureFailPoint(conn, hangFailPointName);
    const deadlineFailPoint = configureFailPoint(conn, deadlineFailPointName);
    const awaitCommandInterrupted = assertCommandFailedWithCodeInParallelShell(
        conn, conn.getDB(dbName), {...command, comment: jsTestName()}, ErrorCodes.Interrupted);

    // Kill the command after the hang fail point is hit.
    hangFailPoint.wait();
    conn.getDB("admin").killOp(findOpid(conn, {"command.comment": jsTestName()}));

    // Release the command and verify that it got interrupted.
    hangFailPoint.off();
    awaitCommandInterrupted();

    // We can disable the deadline fail point now that the command finished.
    // If the command reached the deadline, the server will `tassert`, making the test fail.
    deadlineFailPoint.off();
}

/**
 * Runs a command, which is assumed to run other sub-commands.
 * At fail point `hangFailPointName` inside the sub-command, the main command is killed.
 * Checks that the sub-command is also killed before it reaches fail point `deadlineFailPointName`.
 */
function assertSubCommandKilledAndInterruptsBetweenFailPoints(
    conn, {dbName, command}, subConn, subCmdFilter, hangFailPointName, deadlineFailPointName) {
    jsTestLog("Checking that  " + tojsononeline(command) + " over " + dbName +
              " kills the subcommand matching " + tojsononeline(subCmdFilter) + " between " +
              hangFailPointName + " and " + deadlineFailPointName);

    // Configure the fail points and launch the main command.
    const hangFailPoint = configureFailPoint(subConn, hangFailPointName);
    const deadlineFailPoint = configureFailPoint(subConn, deadlineFailPointName);
    const awaitCommandInterrupted = assertCommandFailedWithCodeInParallelShell(
        conn, conn.getDB(dbName), {...command, comment: jsTestName()}, ErrorCodes.Interrupted);

    // Kill the top-level command after the hang fail point is hit and check for interruption.
    hangFailPoint.wait();
    conn.getDB("admin").killOp(findOpid(conn, {"command.comment": jsTestName()}));
    awaitCommandInterrupted();

    // Release the subcommand and wait for it to be killed.
    const opid = findOpid(subConn, subCmdFilter);
    assert.soon(() => tryFindOpid(subConn, {opid: opid, killPending: true}) != null);
    hangFailPoint.off();
    assert.soon(() => tryFindOpid(subConn, {opid: opid}) == null);

    // We can disable the deadline fail point now that the command finished.
    // If the command reached the deadline, the server will `tassert`, making the test fail.
    deadlineFailPoint.off();
}

// Set up a database with a sharded collection to run checkMetadataConsistency on.
const st = new ShardingTest({shards: 2});

const kDbName = jsTestName(), kCollName = "coll";
assert.commandWorked(
    st.s.adminCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));
st.shardColl(st.s.getDB(kDbName).getCollection(kCollName), {skey: 1});

// Run tests for checkMetadataConsistency on a cluster/DB/collection level.
const checkMetadataForCluster = {
    dbName: 'admin',
    command: {checkMetadataConsistency: 1}
};
const checkMetadataForDb = {
    dbName: kDbName,
    command: {checkMetadataConsistency: 1}
};
const checkMetadataForColl = {
    dbName: kDbName,
    command: {checkMetadataConsistency: kCollName}
};

for (const cmc of [checkMetadataForCluster, checkMetadataForDb, checkMetadataForColl]) {
    // Check that checkMetadataConsistency can be interrupted while establishing cursors on all
    // shards that are the primary shard for one or more databases.
    assertCommandInterruptsBetweenFailPoints(st.s,
                                             cmc,
                                             "hangCheckMetadataBeforeEstablishCursors",
                                             "tripwireCheckMetadataAfterEstablishCursors");

    // Check that _shardsvrCheckMetadataConsistency can be interrupted while taking the DDL lock,
    // which is the main bottleneck when there are concurrent DDL operations.
    assertSubCommandKilledAndInterruptsBetweenFailPoints(
        st.s,
        cmc,
        st.rs0.getPrimary(),
        {"command._shardsvrCheckMetadataConsistency": {$exists: true}},
        "hangShardCheckMetadataBeforeDDLLock",
        "tripwireShardCheckMetadataAfterDDLLock");

    // Check that _shardsvrCheckMetadataConsistency can be interrupted while establishing cursors
    // on all database participants. Since each database is checked sequentially, guaranteeing
    // interruptability here ensures that the command stops working on further databases.
    assertSubCommandKilledAndInterruptsBetweenFailPoints(
        st.s,
        cmc,
        st.rs0.getPrimary(),
        {"command._shardsvrCheckMetadataConsistency": {$exists: true}},
        "hangShardCheckMetadataBeforeEstablishCursors",
        "tripwireShardCheckMetadataAfterEstablishCursors");
}

st.stop();
