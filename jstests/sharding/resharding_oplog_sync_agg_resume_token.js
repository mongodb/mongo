/**
 * Test that the postBatchResumeToken field is only included for the oplog namespace when
 * $_requestResumeToken is specified for an aggregate command.
 *
 * @tags: [requires_fcv_47]
 */
(function() {
"use strict";

var rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

jsTest.log("Inserting documents to generate oplog entries");
let testDB = rst.getPrimary().getDB(dbName);
let testColl = testDB.foo;

for (let i = 0; i < 10; i++) {
    assert.commandWorked(testColl.insert({x: i}));
}

const localDb = rst.getPrimary().getDB("local");

jsTest.log("Run aggregation pipeline on oplog with $_requestResumeToken set");
const resEnabled = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
    $_requestResumeToken: true,
    cursor: {batchSize: 1}
});

assert.commandWorked(resEnabled);
assert(resEnabled.cursor.hasOwnProperty("postBatchResumeToken"), resEnabled);
assert(resEnabled.cursor.postBatchResumeToken.hasOwnProperty("ts"), resEnabled);

jsTest.log("Ensure that postBatchResumeToken attribute is returned for getMore command");
const cursorId = resEnabled.cursor.id;
const resGetMore =
    assert.commandWorked(localDb.runCommand({getMore: cursorId, collection: "oplog.rs"}));

assert.commandWorked(resGetMore);
assert(resGetMore.cursor.hasOwnProperty("postBatchResumeToken"), resGetMore);
assert(resGetMore.cursor.postBatchResumeToken.hasOwnProperty("ts"), resGetMore);

jsTest.log("Run aggregation pipeline on oplog with $_requestResumeToken disabled");
const resDisabled = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
    $_requestResumeToken: false,
    cursor: {}
});

assert.commandWorked(resDisabled);
assert(!resDisabled.cursor.hasOwnProperty("postBatchResumeToken"), resDisabled);

jsTest.log(
    "Run aggregation pipeline on oplog with $_requestResumeToken unspecified and defaulting to disabled");
const resWithout = localDb.runCommand(
    {aggregate: "oplog.rs", pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}], cursor: {}});

assert.commandWorked(resWithout);
assert(!resWithout.cursor.hasOwnProperty("postBatchResumeToken"), resWithout);

jsTest.log("Run aggregation pipeline on non-oplog with $_requestResumeToken set");
const resNotOplog = localDb.runCommand(
    {aggregate: ns, pipeline: [{limit: 100}], $_requestResumeToken: true, cursor: {}});

assert.commandFailedWithCode(resNotOplog, ErrorCodes.FailedToParse);

jsTest.log("Run aggregation pipeline on oplog with empty batch");
const resEmpty = localDb.runCommand({
    aggregate: "oplog.rs",
    pipeline: [{$match: {ts: {$gte: Timestamp(0, 0)}}}],
    $_requestResumeToken: true,
    cursor: {batchSize: 0}
});

assert.commandWorked(resEmpty);
assert(resEmpty.cursor.hasOwnProperty("postBatchResumeToken"), resEmpty);
assert(resEnabled.cursor.postBatchResumeToken.hasOwnProperty("ts"), resEmpty);
assert.eq(resEmpty.cursor.postBatchResumeToken.ts, new Timestamp(0, 0));

jsTest.log("End of test");

rst.stopSet();
})();