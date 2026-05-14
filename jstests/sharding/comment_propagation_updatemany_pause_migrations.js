/*
 * SERVER-126535: Repro for the `comment` field being silently dropped when an
 * updateMany / deleteMany / bulkWrite(updateMany) travels through the
 * pause-migrations coordinator path (_shardsvrCoordinateMultiUpdate).
 *
 * filterRequestGenericArguments() in coordinate_multi_update_util.cpp resets the
 * generic arguments and only restores rawData. As a result the comment never
 * reaches the shard executing the write and does not appear in currentOp /
 * slow query logs on the shard.
 *
 * This test pauses the multi-update coordinator on the shard primary while the
 * write is running, then inspects $currentOp on each shard for the comment.
 * The test FAILS in the buggy state (comment not found on the shard) and is
 * expected to PASS once the fix forwards `comment` (and other shard-forwarded
 * generic arguments) through the coordinator.
 *
 * @tags: [
 *  requires_fcv_80,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: {rs0: {nodes: 1}}});
const dbName = "test";
const collName = "coll";

const mongosColl = st.s0.getDB(dbName).getCollection(collName);
assert.commandWorked(mongosColl.insertMany([
    {_id: 1, member: "abc123", points: 0},
    {_id: 2, member: "abc123", points: 100},
]));

// Enable the pause-migrations multi-update path and force mongos to refresh.
assert.commandWorked(st.s.adminCommand(
    {setClusterParameter: {pauseMigrationsDuringMultiUpdates: {enabled: true}}}));
assert.commandWorked(st.s.adminCommand({getClusterParameter: "pauseMigrationsDuringMultiUpdates"}));

const shardPrimary = st.rs0.getPrimary();
const shardAdmin = shardPrimary.getDB("admin");

/**
 * Returns true iff $currentOp on the shard primary has at least one op whose
 * `command.comment` matches the expected value. Inspects every op (idle +
 * active, all users, all namespaces) so we observe the write even while it's
 * blocked inside the coordinator.
 */
function commentObservedOnShard(expectedComment) {
    const ops = shardAdmin
        .aggregate([
            {$currentOp: {allUsers: true, idleConnections: true, idleSessions: true}},
            {$match: {"command.comment": expectedComment}},
        ])
        .toArray();
    if (ops.length > 0) {
        jsTest.log("Shard ops carrying comment=" + expectedComment + ": " + tojson(ops));
    }
    return ops.length > 0;
}

function runCase(label, expectedComment, clientFn) {
    jsTest.log("=== " + label + " (comment=" + expectedComment + ") ===");

    const fp = configureFailPoint(shardPrimary.getDB(dbName), "hangDuringMultiUpdateCoordinatorRun");

    const joinShell = startParallelShell(
        funWithArgs(clientFn, dbName, collName, expectedComment),
        st.s0.port,
    );

    // Wait for the coordinator to be paused on the shard, then scan currentOp.
    fp.wait();

    const found = commentObservedOnShard(expectedComment);

    fp.off();
    joinShell();

    assert(found,
           label + ": expected `comment=" + expectedComment +
               "` to appear in $currentOp on the shard primary, " +
               "but the comment was dropped by the pause-migrations coordinator. " +
               "See SERVER-126535 (filterRequestGenericArguments in " +
               "coordinate_multi_update_util.cpp strips all generic args except rawData).");
}

runCase("updateMany via update cmd",
        "server126535-updatemany",
        function(dbName, collName, expectedComment) {
            const res = db.getSiblingDB(dbName).runCommand({
                update: collName,
                updates: [{q: {member: "abc123"}, u: {$set: {points: 50}}, multi: true}],
                comment: expectedComment,
            });
            assert.commandWorked(res);
            assert.eq(res.nModified, 2, tojson(res));
        });

runCase("bulkWrite updateMany",
        "server126535-bulkwrite",
        function(dbName, collName, expectedComment) {
            const res = db.adminCommand({
                bulkWrite: 1,
                ops: [{
                    update: 0,
                    filter: {member: "abc123"},
                    updateMods: {$set: {points: 75}},
                    multi: true,
                }],
                nsInfo: [{ns: dbName + "." + collName}],
                comment: expectedComment,
            });
            assert.commandWorked(res);
        });

runCase("deleteMany via delete cmd",
        "server126535-deletemany",
        function(dbName, collName, expectedComment) {
            const res = db.getSiblingDB(dbName).runCommand({
                delete: collName,
                deletes: [{q: {member: "abc123"}, limit: 0}],
                comment: expectedComment,
            });
            assert.commandWorked(res);
            assert.eq(res.n, 2, tojson(res));
        });

st.stop();
