/**
 * Tests the behavior of $merge being run on a secondary.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

const primary = replTest.getPrimary().getDB("test");
const secondary = replTest.getSecondary().getDB("test");
assert.commandWorked(primary.setProfilingLevel(2));

const inputCollPrimary = primary.getCollection("inputColl");
const outColl = primary.getCollection("outColl");

const replSetConn = new Mongo(replTest.getURL());
replSetConn.setReadPref("secondary");
const db = replSetConn.getDB("test");
const inputColl = db["inputColl"];

assert.commandWorked(inputCollPrimary.insert({_id: 0, a: 1}, {writeConcern: {w: 2}}));
assert.commandWorked(inputCollPrimary.insert({_id: 1, a: 2}, {writeConcern: {w: 2}}));

// Make sure $merge succeeds with all combinations of merge modes in which it is expected to.
withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    // Skip when whenNotMatchedMode is 'fail' since the output collection is empty, so this
    // will cause the aggregation to fail.
    if (whenNotMatchedMode === "fail") {
        return;
    }

    const commentStr = "whenMatched_" + whenMatchedMode + "_whenNotMatched_" + whenNotMatchedMode;
    assert.eq(0,
              inputColl
                  .aggregate([{
                                 $merge: {
                                     into: outColl.getName(),
                                     whenMatched: whenMatchedMode,
                                     whenNotMatched: whenNotMatchedMode
                                 }
                             }],
                             {comment: commentStr})
                  .itcount());
    assert.eq(whenNotMatchedMode === "discard" ? 0 : 2, outColl.find().itcount());
    if (whenMatchedMode === "fail") {
        assert.eq(
            1,
            primary.system.profile.find({"op": "insert", "command.comment": commentStr}).itcount());
    } else {
        assert.eq(
            2,
            primary.system.profile.find({"op": "update", "command.comment": commentStr}).itcount());
    }
    outColl.drop();
});

// Verify that writeErrors are promoted to top-level errors.
outColl.drop({writeConcern: {w: 2}});
assert.commandWorked(outColl.insert({a: 2}, {writeConcern: {w: 2}}));
const pipeline = [{$merge: {into: "outColl", whenMatched: "fail", whenNotMatched: "insert"}}];
const res = secondary.runCommand({aggregate: outColl.getName(), pipeline: pipeline, cursor: {}});
assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
assert(!res.hasOwnProperty("writeErrors"));
assert(!res.hasOwnProperty("writeConcernError"));

replTest.stopSet();
})();
