/**
 *
 * Runs a number of shard split and moveChunk to compare the time it takes for both operations.
 * @tags: [requires_fcv_52, featureFlagShardSplit]
 */

load("jstests/serverless/libs/basic_serverless_test.js");
load("jstests/replsets/rslib.js");

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

    const kMoveChunkLog = "ctx\":\"MoveChunk\",\"msg\":\"Exiting commit critical section";
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

    const kEnterBlockingState = "Entering 'blocking' state.";
    const kWaitingForCatchup = "Waiting for recipient nodes to reach block timestamp.";
    const kApplyingSplitConfig = "Applying the split config.";
    const kTriggeringRecipientElection =
        "Triggering an election after recipient has accepted the split.";
    const kWaitForMajorityWrite = "Waiting for majority commit on recipient primary";
    const kCommitted = "Entering 'committed' state.";
    const kDecisionReached = "Shard split decision reached";

    const report = {
        // shard split critical section:
        //  - wait to abort index builds
        waitToAbortIndexBuilds: extractTs(checkLog.getLogMessage(primary, kWaitingForCatchup)) -
            extractTs(checkLog.getLogMessage(primary, kEnterBlockingState)),
        //  - wait for recipient nodes to catch up with blockTimestamp
        waitForRecipientCatchup: extractTs(checkLog.getLogMessage(primary, kApplyingSplitConfig)) -
            extractTs(checkLog.getLogMessage(primary, kWaitingForCatchup)),
        //  - wait for split acceptance
        waitForSplitAcceptance:
            extractTs(checkLog.getLogMessage(primary, kTriggeringRecipientElection)) -
            extractTs(checkLog.getLogMessage(primary, kApplyingSplitConfig)),
        //  - wait for recipient primary to step up
        waitForRecipientStepUp: extractTs(checkLog.getLogMessage(primary, kWaitForMajorityWrite)) -
            extractTs(checkLog.getLogMessage(primary, kTriggeringRecipientElection)),
        //  - wait for recipient to majority commit write
        waitForRecipientMajorityWrite: extractTs(checkLog.getLogMessage(primary, kCommitted)) -
            extractTs(checkLog.getLogMessage(primary, kWaitForMajorityWrite)),
        //  - total duration
        totalDuration: extractTs(checkLog.getLogMessage(primary, kDecisionReached)) -
            extractTs(checkLog.getLogMessage(primary, kEnterBlockingState)),
        totalDurationWithoutCatchup: extractTs(checkLog.getLogMessage(primary, kDecisionReached)) -
            extractTs(checkLog.getLogMessage(primary, kApplyingSplitConfig)),
    };

    jsTestLog(`Shard split performance report: ${tojson(report)}`);

    // Validate using metrics from log output
    const maxCriticalSectionDurationWithoutCatchup = 500;
    assert.lt(report.totalDurationWithoutCatchup,
              maxCriticalSectionDurationWithoutCatchup,
              `The total duration without recipient catchup must be less than ${
                  maxCriticalSectionDurationWithoutCatchup}ms`);

    // Validate using reported values in serverStatus
    const donorPrimary = test.donor.getPrimary();
    const serverStatus = assert.commandWorked(donorPrimary.adminCommand({serverStatus: 1}));
    const splitStats = serverStatus.shardSplits;
    assert.eq(splitStats.totalCommitted, 1);
    assert.lt(splitStats.totalCommittedDurationWithoutCatchupMillis,
              maxCriticalSectionDurationWithoutCatchup,
              `The total duration without recipient catchup must be less than ${
                  maxCriticalSectionDurationWithoutCatchup}ms`);

    test.stop();
}

runOneMoveChunk();
runOneSplit();
