/**
 * Tests $_requestResumeToken in aggregate command.
 *
 * @tags: [
 *  requires_fcv_72,
 *  featureFlagReshardingImprovements
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const collName = 'foo';
const mongos = st.s0;
const numInitialDocs = 10;
const db = st.rs0.getPrimary().getDB(kDbName);

let bulk = db.getCollection(collName).initializeOrderedBulkOp();
for (let x = 0; x < numInitialDocs; x++) {
    bulk.insert({oldKey: x, newKey: numInitialDocs - x});
}
assert.commandWorked(bulk.execute());

if (!FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements")) {
    jsTestLog("Skipping test since featureFlagReshardingImprovements is not enabled.");
    quit();
}

jsTest.log("aggregate with $requestResumeToken should fail without hint: {$natural: 1}.");
assert.commandFailedWithCode(
    db.runCommand({aggregate: collName, pipeline: [], $_requestResumeToken: true, cursor: {}}),
    ErrorCodes.BadValue);

jsTest.log("aggregate with $requestResumeToken should fail if the hint is not {$natural: 1}.");
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [],
    $_requestResumeToken: true,
    cursor: {},
    hint: {oldKey: 1}
}),
                             ErrorCodes.BadValue);

jsTest.log(
    "aggregate with $requestResumeToken should return PBRT with recordId and an initialSyncId.");
let res = db.runCommand({
    aggregate: collName,
    pipeline: [],
    $_requestResumeToken: true,
    hint: {$natural: 1},
    cursor: {batchSize: 1}
});
assert.hasFields(res.cursor, ["postBatchResumeToken"]);
assert.hasFields(res.cursor.postBatchResumeToken, ["$recordId"]);
assert.hasFields(res.cursor.postBatchResumeToken, ["$initialSyncId"]);
const resumeToken = res.cursor.postBatchResumeToken;

jsTest.log("aggregate with wrong $recordId type in $resumeAfter should fail");
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [],
    hint: {$natural: 1},
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: 1, $initialSyncId: UUID("81fd5473-1747-4c9d-8743-f10642b3bb99")},
    cursor: {batchSize: 1}
}),
                             ErrorCodes.BadValue);

jsTest.log("aggregate with $resumeAfter should fail without {$_requestResumeToken: true}.");
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [],
    hint: {$natural: 1},
    $_resumeAfter: resumeToken,
    cursor: {batchSize: 1}
}),
                             ErrorCodes.BadValue);

res = db.runCommand({
    aggregate: collName,
    pipeline: [],
    $_requestResumeToken: true,
    hint: {$natural: 1},
    $_resumeAfter: resumeToken,
    cursor: {batchSize: 1}
});
st.stop();
