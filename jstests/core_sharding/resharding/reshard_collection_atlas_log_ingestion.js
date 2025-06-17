/**
 * Atlas ingests the resharding complete log with id 7763800 for analytics purposes. These tests
 * ensure metrics are reported as expected.
 * @tags: [
 *  uses_atclustertime,
 *  # This test performs explicit calls to shardCollection.
 *  assumes_unsharded_collection,
 *  # The most reliable way to get resharding to fail with a runtime error is by having a duplicate
 *  #_id across two different shards. We need 2 or more shards and no balancer to ensure this.
 *  requires_2_or_more_shards,
 *  assumes_balancer_off,
 *  # Stepdowns should be fine, but the only suites core_sharding tests run in randomize between
 *  # stepdowns, terminates, and kills. Fetching the completion logs with getLog cannot tolerate
 *  # node restarts.
 *  does_not_support_stepdowns,
 * ]
 */

import {
    withSkipRetryOnNetworkError
} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {createChunks, getShardNames} from "jstests/sharding/libs/sharding_util.js";

const dbName = db.getName();
const oldShardKey = {
    oldKey: 1
};
const newShardKey = {
    newKey: 1
};

main();

function main() {
    {
        // Success case.
        const collName = `${jsTestName()}-success`;
        initializeCollection(collName, false);
        const uuid = UUID();
        assert.commandWorked(runResharding(collName, uuid));
        const logs = getCompletionLogs(uuid);
        verifyCompletionLogs(collName, logs, "success");
    }

    {
        // Failure case.
        const collName = `${jsTestName()}-failed`;
        initializeCollection(collName, true);
        const uuid = UUID();
        assert.commandFailed(runResharding(collName, uuid));
        const logs = getCompletionLogs(uuid);
        verifyCompletionLogs(collName, logs, "failed");
    }
}

function initializeCollection(collName, includeDuplicateKey) {
    const docCount = 1000;
    const minKey = 0;
    const maxKey = docCount;
    const coll = db.getCollection(collName);
    const chunks = createChunks(getShardNames(db), "oldKey", minKey, maxKey);
    CreateShardedCollectionUtil.shardCollectionWithChunks(coll, oldShardKey, chunks);
    const docs = [];
    for (let i = 0; i < docCount; i++) {
        docs.push({_id: i, oldKey: i, newKey: maxKey - i});
    }
    if (includeDuplicateKey) {
        const firstDoc = docs[0];
        const lastDoc = docs[docs.length - 1];
        lastDoc._id = firstDoc._id;
        lastDoc.newKey = firstDoc.newKey;
    }
    assert.commandWorked(coll.insert(docs));
}

function runResharding(collName, uuid) {
    jsTestLog(`Running resharding with user supplied UUID: ${tojson(uuid)}`);
    return db.adminCommand(
        {reshardCollection: `${dbName}.${collName}`, key: newShardKey, reshardingUUID: uuid});
}

function getLogs(connection) {
    // The retry logic in network_error_and_txn_override.js refuses to handle getLog commands,
    // because after a failover a test might naively target a different node. This test doesn't care
    // about which node has the log and checks each of them, so it uses its own retry logic to
    // handle this command.
    let logs;
    withSkipRetryOnNetworkError(() => {
        assert.soon(() => {
            try {
                logs = assert.commandWorked(connection.adminCommand({getLog: "global"})).log;
                return true;
            } catch (e) {
                jsTestLog(`Failed to fetch logs: ${tojson(e)}`);
                return false;
            }
        });
    });
    return logs;
}

function getCompletionLogs(uuid) {
    const topology = DiscoverTopology.findConnectedNodes(db);
    const configNodes = topology.configsvr.nodes;
    const logs = [];
    for (const node of configNodes) {
        logs.push(...getLogs(new Mongo(node)));
    }
    return logs.filter(log => log.includes(`"id":7763800,`)).filter(log => {
        return log.includes(extractUUIDFromObject(uuid));
    });
}

function verifyCompletionLogs(collName, logs, expectedStatus) {
    // There could be multiple completion logs if deleting the coordinator state document is rolled
    // back.
    assert.gt(logs.length,
              0,
              "No log lines with id 7763800 emitted on config server following resharding.");
    const completionLog = JSON.parse(logs.pop());
    jsTestLog(`Completion log found: ${tojson(completionLog)}`);
    const info = completionLog.attr.info;
    const stats = info.statistics;
    assert.eq(info.status, expectedStatus);
    const success = info.status == "success";
    assert(info.hasOwnProperty("userSuppliedUUID"), "Missing userSuppliedUUID");
    if (!success) {
        assert(info.hasOwnProperty("failureReason"), "Missing failureReason");
    }
    assert.eq(stats.ns, `${dbName}.${collName}`, "namespace");
    assert.gt(stats.operationDurationMs, 0, "operationDurationMs");
    assert.eq(stats.provenance, "reshardCollection");
    assert.gt(stats.numberOfSourceShards, 0, "numberOfSourceShards");
    assert.gt(stats.numberOfDestinationShards, 0, "numberOfDestinationShards");
    verifyDonorMetrics(stats, success);
    verifyRecipientMetrics(stats, success);
    verifyTotals(stats, success);
}

function verifyDonorMetrics(stats, success) {
    assert(stats.hasOwnProperty("donors"), "Missing donors");
    assert.gt(Object.keys(stats.donors).length, 0, "No donors reported");
    for (const donor of Object.values(stats.donors)) {
        assert(donor.hasOwnProperty("shardName"), "Missing donor shardName");
        assert.gt(donor.bytesToClone, 0, "bytesToClone");
        assert.gt(donor.documentsToClone, 0, "documentsToClone");
        assert(donor.hasOwnProperty("writesDuringCriticalSection"),
               "Missing writesDuringCriticalSection");
        if (success) {
            assert(donor.hasOwnProperty("phaseDurations"), "Missing donor phaseDurations");
            assert.gte(
                donor.phaseDurations.criticalSectionDurationMs, 0, "criticalSectionDurationMs");
        }
    }
}

function verifyRecipientMetrics(stats, success) {
    assert(stats.hasOwnProperty("recipients"), "Missing recipients");
    assert.gt(Object.keys(stats.recipients).length, 0, "No recipients reported");
    let recipientTotalBytes = 0;
    let recipientTotalDocs = 0;
    let recipientTotalFetched = 0;
    let recipientTotalApplied = 0;
    for (const recipient of Object.values(stats.recipients)) {
        assert(recipient.hasOwnProperty("shardName"), "Missing recipient shardName");
        recipientTotalBytes += recipient.bytesCloned;
        recipientTotalDocs += recipient.documentsCloned;
        recipientTotalFetched += recipient.oplogsFetched;
        recipientTotalApplied += recipient.oplogsApplied;
        assert(recipient.hasOwnProperty("phaseDurations"), "Missing recipient phaseDurations");
        for (const [phase, duration] of Object.entries(recipient.phaseDurations)) {
            assert.gt(duration, 0, `Phase ${phase} had no duration`);
        }
        if (success) {
            for (const expected
                     of ["copyDurationMs", "applyDurationMs", "buildingIndexDurationMs"]) {
                assert(recipient.phaseDurations.hasOwnProperty(expected),
                       `Successful operation did not report ${expected}`);
            }
            assert.gt(recipient.indexCount, 0, "indexCount");
        }
    }
    assert.gt(recipientTotalBytes, 0, "recipientTotalBytes");
    assert.gt(recipientTotalDocs, 0, "recipientTotalDocs");
    assert.gt(recipientTotalFetched, 0, "recipientTotalFetched");
    if (success) {
        assert.gt(recipientTotalApplied, 0, "recipientTotalApplied");
    }
}

function verifyTotals(stats, success) {
    assert(stats.hasOwnProperty("totals"), "Missing totals");
    let totals = stats.totals;
    assert.gt(totals.copyDurationMs, 0, "copyDurationMs");
    if (success) {
        assert.gt(totals.applyDurationMs, 0, "applyDurationMs");
        assert.gt(totals.criticalSectionDurationMs, 0, "criticalSectionDurationMs");
    }

    assert.gt(totals.totalBytesToClone, 0, "totalBytesToClone");
    assert.gt(totals.totalDocumentsToClone, 0, "totalDocumentsToClone");
    assert.gt(totals.averageDocSize, 0, "averageDocSize");
    assert(totals.hasOwnProperty("totalWritesDuringCriticalSection"),
           "Missing totalWritesDuringCriticalSection");

    assert.gt(totals.totalBytesCloned, 0, "totalBytesCloned");
    assert.gt(totals.totalDocumentsCloned, 0, "totalDocumentsCloned");
    assert.gt(totals.totalOplogsFetched, 0, "totalOplogsFetched");
    if (success) {
        assert.gt(totals.totalOplogsApplied, 0, "totalOplogsApplied");
    }
    assert.gt(totals.maxRecipientIndexes, 0, "maxRecipientIndexes");
}
