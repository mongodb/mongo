/**
 * Tests that the resharding operation will fail if a recipient shard would have missed oplog
 * entries from a donor shard.
 * @tags: [
 *   requires_fcv_47,
 * ]
 */
(function() {
"use strict";

const rst = new ReplSetTest({
    // Set the syncdelay to 1s to speed up checkpointing.
    nodeOptions: {syncdelay: 1},
    nodes: 1
});
// Set max oplog size to 1MB.
rst.startSet({oplogSize: 1});
rst.initiate();

jsTest.log("Inserting documents to generate oplog entries");
let testDB = rst.getPrimary().getDB("test");
let testColl = testDB.foo;
const localDb = rst.getPrimary().getDB("local");

// 400KB each so that oplog can keep at most two insert oplog entries.
const longString = new Array(400 * 1024).join("a");

// fill the oplog with two large documents
assert.commandWorked(testColl.insert({_id: 0, longString: longString}));
assert.commandWorked(testColl.insert({_id: 1, longString: longString}));

let oplogEntry = localDb.oplog.rs.findOne({"op": "i", "o._id": 0});

jsTest.log("Run aggregation pipeline on oplog with $_requestReshardingResumeToken set");
assert.commandWorked(localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: oplogEntry.ts}}}],
    $_requestReshardingResumeToken: true,
    cursor: {}
}));

let id = 2;
assert.soon(() => {
    // keep inserting documents until the oplog truncates
    assert.commandWorked(testColl.insert({_id: id, longString: longString}));
    id++;
    return timestampCmp(localDb.oplog.rs.findOne().ts, oplogEntry.ts) == 1;
}, "Timeout waiting for oplog to roll over on primary");

assert.commandFailedWithCode(localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: oplogEntry.ts}}}],
    $_requestReshardingResumeToken: true,
    cursor: {}
}),
                             ErrorCodes.OplogQueryMinTsMissing);

jsTest.log(
    "Run aggregation pipeline on incomplete oplog with $_requestReshardingResumeToken set to false");
assert.commandWorked(localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: oplogEntry.ts}}}],
    $_requestReshardingResumeToken: false,
    cursor: {}
}));

jsTest.log("Run non-$gte oplog aggregation pipeline with $_requestReshardingResumeToken set");
assert.commandFailedWithCode(localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {"op": "i"}}],
    $_requestReshardingResumeToken: true,
    cursor: {}
}),
                             ErrorCodes.InvalidOptions);

jsTest.log("End of test");

rst.stopSet();
})();
