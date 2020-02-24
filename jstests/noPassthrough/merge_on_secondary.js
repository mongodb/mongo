/**
 * Tests the behavior of $merge being run on a secondary.
 *
 * @tags: [assumes_unsharded_collection, requires_replication, requires_spawning_own_processes]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.

let replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();
replTest.awaitReplication();

let primary = replTest.getPrimary().getDB("test");
let secondary = replTest.getSecondary().getDB("test");
assert.commandWorked(primary.setProfilingLevel(2));
secondary.getMongo().setReadPref("secondary");

const inputCollPrimary = primary.getCollection("inputColl");
const inputCollSecondary = secondary.getCollection("inputColl");
const outColl = primary.getCollection("outColl");

assert.commandWorked(inputCollPrimary.insert({_id: 0, a: 1}, {writeConcern: {w: 2}}));
assert.commandWorked(inputCollPrimary.insert({_id: 1, a: 2}, {writeConcern: {w: 2}}));

// Make sure $merge succeeds with all combinations of merge modes in which it is expected to.
withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    // Skip when whenNotMatchedMode is 'fail' since the output collection is empty, so this
    // will cause the aggregation to fail.
    if (whenNotMatchedMode == "fail") {
        return;
    }

    const commentStr = "whenMatched_" + whenMatchedMode + "_whenNotMatched_" + whenNotMatchedMode;
    assert.eq(0,
              inputCollSecondary
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
    if (whenMatchedMode === "fail" && whenNotMatchedMode === "insert") {
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

replTest.stopSet();
})();