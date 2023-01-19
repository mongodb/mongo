/**
 *
 * Runs a number of shard split and moveChunk to compare the time it takes for both operations.
 * @tags: [requires_fcv_62, serverless]
 */

import {assertMigrationState, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

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

function printLog(connection) {
    const log = cat(connection.fullOptions.logFile);
    jsTestLog(`Printing log for ${connection}`);
    for (let line of log.split("\n")) {
        print(`d${connection.port} | ${line}`);
    }

    return log;
}

function extractTimingsForSplitSteps(connection) {
    const log = printLog(connection);
    const logLines = {
        enterBlockingState: "Entering 'blocking' state.",
        waitingForCatchup: "Waiting for recipient nodes to reach block timestamp.",
        applyingSplitConfig: "Applying the split config.",
        triggeringRecipientElection:
            "Triggering an election after recipient has accepted the split.",
        waitForMajorityWrite: "Waiting for majority commit on recipient primary",
        committed: "Entering 'committed' state.",
        decisionReached: "Shard split decision reached",
    };

    const result = {};

    for (let line of log.split("\n")) {
        for (let key in logLines) {
            if (line.includes(logLines[key])) {
                result[key] = extractTs(line);
                break;
            }
        }
    }

    return result;
}

function runOneSplit() {
    "use strict";

    const test = new ShardSplitTest({
        quickGarbageCollection: true,
        nodeOptions: {
            useLogFiles: true,
            setParameter: {logComponentVerbosity: tojson({replication: 4, command: 4})}
        }
    });
    test.addAndAwaitRecipientNodes();

    const tenantIds = [ObjectId(), ObjectId()];
    const operation = test.createSplitOperation(tenantIds);
    assert.commandWorked(operation.commit());
    test.removeRecipientNodesFromDonor();

    const donorPrimary = test.donor.getPrimary();

    assertMigrationState(donorPrimary, operation.migrationId, "committed");

    const result = extractTimingsForSplitSteps(donorPrimary);
    test.donor.getSecondaries().forEach(node => {
        printLog(node);
    });

    const report = {
        // shard split critical section:
        //  - wait to abort index builds
        waitToAbortIndexBuilds: result.waitingForCatchup - result.enterBlockingState,
        //  - wait for recipient nodes to catch up with blockTimestamp
        waitForRecipientCatchup: result.applyingSplitConfig - result.waitingForCatchup,
        //  - wait for split acceptance
        waitForSplitAcceptance: result.triggeringRecipientElection - result.applyingSplitConfig,
        //  - wait for recipient primary to step up
        waitForRecipientStepUp: result.waitForMajorityWrite - result.triggeringRecipientElection,
        //  - wait for recipient to majority commit write
        waitForRecipientMajorityWrite: result.committed - result.waitForMajorityWrite,
        //  - total duration
        totalDuration: result.decisionReached - result.enterBlockingState,
        totalDurationWithoutCatchup: result.decisionReached - result.applyingSplitConfig,
    };

    jsTestLog(`Shard split performance report: ${tojson(report)}`);

    // Validate using metrics from log output
    const maxCriticalSectionDurationWithoutCatchup = 500;
    assert.lt(report.totalDurationWithoutCatchup,
              maxCriticalSectionDurationWithoutCatchup,
              `The total duration without recipient catchup must be less than ${
                  maxCriticalSectionDurationWithoutCatchup}ms`);

    // Validate using reported values in serverStatus
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
