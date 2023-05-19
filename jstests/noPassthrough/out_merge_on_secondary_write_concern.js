/**
 * Tests that the writeConcern of a $out/$merge executed on a secondary is propagated to the primary
 * and is properly respected.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/write_concern_util.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();
const primaryDB = primary.getDB("test");
const secondaryDB = secondary.getDB("test");
// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

assert.commandWorked(primaryDB.setProfilingLevel(2));
secondaryDB.getMongo().setReadPref("secondary");

const inputCollPrimary = primaryDB.getCollection("inputColl");
const inputCollSecondary = secondaryDB.getCollection("inputColl");
const outColl = primaryDB.getCollection("outColl");

assert.commandWorked(inputCollPrimary.insert({_id: 0, a: 1}, {writeConcern: {w: 2}}));
assert.commandWorked(inputCollPrimary.insert({_id: 1, a: 2}, {writeConcern: {w: 2}}));

function testWriteConcern(pipeline, comment) {
    outColl.drop({writeConcern: {w: 2}});

    assert.eq(
        0,
        inputCollSecondary.aggregate(pipeline, {writeConcern: {w: 2}, comment: comment}).itcount());

    // Verify that the command sent to the primary has the expected w:2 writeConcern attached to it.
    const arr =
        primaryDB.system.profile.find({"op": "insert", "command.comment": comment}).toArray();
    const expectedWriteConcern = {w: 2, wtimeout: 0, provenance: "clientSupplied"};
    assert.eq(1, arr.length);
    assert.eq(expectedWriteConcern, arr[0].command.writeConcern);
    outColl.drop({writeConcern: {w: 2}});

    // Stop the oplog fetcher on the secondary.
    stopServerReplication(secondary);

    const res = secondaryDB.runCommand({
        aggregate: "inputColl",
        pipeline: pipeline,
        writeConcern: {w: 2, wtimeout: 1000},
        comment: comment + "_fail",
        cursor: {}
    });
    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
    assert(!res.hasOwnProperty("writeErrors"));
    assert(!res.hasOwnProperty("writeConcernError"));

    restartServerReplication(secondary);
}

const mergePipeline =
    [{$merge: {into: outColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}];
testWriteConcern(mergePipeline, "merge_on_secondary_write_concern");

const outPipeline = [{$group: {_id: "$_id", sum: {$sum: "$a"}}}, {$out: outColl.getName()}];
testWriteConcern(outPipeline, "out_on_secondary_write_concern");

replTest.stopSet();
})();
