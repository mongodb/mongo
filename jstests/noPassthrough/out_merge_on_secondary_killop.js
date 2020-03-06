/**
 * Tests that when killOp is run on the insert/update of a $out/$merge run on a secondary, the
 * original aggregate command is killed as well. Likewise, tests that when killOp is run on the
 * $out/$merge command on the secondary, no further insert/update batches are sent to the primary.
 *
 * @tags: [assumes_unsharded_collection, requires_replication]
 */
(function() {
"use strict";

const kDBName = "out_merge_on_secondary_db";

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const primaryDB = primary.getDB(kDBName);
const secondaryDB = secondary.getDB(kDBName);
assert.commandWorked(primaryDB.setProfilingLevel(2));
secondaryDB.getMongo().setReadPref("secondary");

const inputColl = primaryDB.getCollection("inputColl");
const outColl = primaryDB.getCollection("outColl");

assert.commandWorked(inputColl.insert({_id: 0, a: 1}, {writeConcern: {w: 2}}));
assert.commandWorked(inputColl.insert({_id: 1, a: 2}, {writeConcern: {w: 2}}));

function enableHangFailPoints() {
    configureFailPoint(primary, "hangDuringBatchInsert");
    configureFailPoint(primary, "hangDuringBatchUpdate");
}

function disableHangFailPoints() {
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "hangDuringBatchInsert", mode: "off"}));
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "hangDuringBatchUpdate", mode: "off"}));
}

/**
 * Finds and kills the operation on the given connection marked with the given comment.
 */
function findAndKillOp(conn, comment) {
    assert.soon(() => {
        const curOps =
            conn.getDB("admin")
                .aggregate([{$currentOp: {allUsers: true}}, {$match: {"command.comment": comment}}])
                .toArray();
        if (curOps.length === 0) {
            return false;
        }
        assert.eq(curOps.length, 1);
        assert.commandWorked(conn.getDB(kDBName).killOp(curOps[0].opid));
        return true;
    });
}

function testKillOp(pipeline, comment) {
    enableHangFailPoints();

    // Run the aggregate and ensure that it is interrupted.
    const runAggregate = `
            const testDB = db.getSiblingDB("${kDBName}");
            testDB.setSlaveOk(true);
            const res = testDB.runCommand({
                aggregate: "inputColl",
                pipeline: ${tojson(pipeline)},
                writeConcern: {w: 2},
                comment: "${comment}",
                cursor: {}
            });
            assert.commandFailedWithCode(res, ErrorCodes.Interrupted);
        `;
    let awaitShell = startParallelShell(runAggregate, secondary.port);

    // Find and kill the insert/update on the primary corresponding to the aggregate on the
    // secondary.
    findAndKillOp(primary, comment);

    disableHangFailPoints();
    awaitShell();
    enableHangFailPoints();

    awaitShell = startParallelShell(runAggregate, secondary.port);

    // Find and kill the aggregate on the secondary while it is still waiting on the primary.
    findAndKillOp(secondary, comment);

    disableHangFailPoints();
    awaitShell();
}

const mergePipeline = [{$merge: {into: "outColl"}}];
testKillOp(mergePipeline, "merge_on_secondary_killop");

const outPipeline = [{$group: {_id: "$_id", sum: {$sum: "$a"}}}, {$out: outColl.getName()}];
testKillOp(outPipeline, "out_on_secondary_killop");

replTest.stopSet();
}());