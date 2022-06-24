/**
 *
 * Runs a number of shard split and moveChunk to compare the time it takes for both operations.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/serverless/libs/basic_serverless_test.js");
load("jstests/replsets/rslib.js");

const kBlockStart = "Entering 'blocking' state.";
const kReconfig = "Applying the split config";
const kWaitForRecipients = "Waiting for recipient to accept the split.";
const kEndMsg = "Shard split decision reached";
const kMoveChunkLog = "ctx\":\"MoveChunk\",\"msg\":\"Exiting commit critical section";

function runOneMoveChunk() {
    'use strict';

    load("jstests/sharding/libs/find_chunks_util.js");

    var st = new ShardingTest({mongos: 1, shards: 2});
    var kDbName = 'db';

    var mongos = st.s0;
    var shard0 = st.shard0.shardName;
    var shard1 = st.shard1.shardName;

    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, shard0);

    const keyDoc = {a: 1};
    var ns = kDbName + '.foo';

    // Fail if find is not a valid shard key.
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: keyDoc}));

    var chunkId = findChunksUtil.findOneChunkByNs(mongos.getDB('config'), ns, {shard: shard0})._id;

    assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: keyDoc, to: shard1}));
    assert.eq(shard1, mongos.getDB('config').chunks.findOne({_id: chunkId}).shard);

    const moveMsg = checkLog.getLogMessage(st.shard0, kMoveChunkLog);
    assert(moveMsg);

    const moveJson = JSON.parse(moveMsg);
    const duration = parseInt(moveJson.attr.durationMillis, 10);

    mongos.getDB(kDbName).foo.drop();

    st.stop();

    jsTestLog(`moveChunk critical section lasted ${duration} ms`);

    return duration;
}

function extractTs(message) {
    assert(message);
    const msgJson = JSON.parse(message);
    return Date.parse(msgJson.t["$date"]);
}

function runOneSplit() {
    "use strict";

    const recipientTagName = "recipientNode";
    const recipientSetName = "recipientSetName";
    const test =
        new BasicServerlessTest({recipientTagName, recipientSetName, quickGarbageCollection: true});

    test.addRecipientNodes();
    test.donor.awaitSecondaryNodes();

    const primary = test.donor.getPrimary();

    const tenantIds = ["tenant1", "tenant2"];
    const operation = test.createSplitOperation(tenantIds);
    assert.commandWorked(operation.commit());

    test.removeRecipientNodesFromDonor();
    assertMigrationState(test.donor.getPrimary(), operation.migrationId, "committed");

    const blockTS = extractTs(checkLog.getLogMessage(primary, kBlockStart));
    const reconfigTS = extractTs(checkLog.getLogMessage(primary, kReconfig));
    const waitForRecipientsTS = extractTs(checkLog.getLogMessage(primary, kWaitForRecipients));
    const endTS = extractTs(checkLog.getLogMessage(primary, kEndMsg));

    const blockDurationMs = endTS - blockTS;
    const waitForRecipientsDurationMs = endTS - waitForRecipientsTS;
    const reconfigDurationMs = endTS - reconfigTS;

    const splitResult = {blockDurationMs, reconfigDurationMs, waitForRecipientsDurationMs};

    jsTestLog(`Performance result of shard split: ${tojson(splitResult)}`);
    const maximumReconfigDuration = 500;
    assert.lt(reconfigDurationMs,
              maximumReconfigDuration,
              `The reconfig critical section of split must be below ${maximumReconfigDuration}ms`);

    test.stop();
}

runOneMoveChunk();
runOneSplit();
