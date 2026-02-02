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
 *  # stepdowns, terminates, and kills. Failpoints and getLog cannot tolerate node restarts.
 *  does_not_support_stepdowns,
 *  # resharding can't run during FCV transitions
 *  cannot_run_during_upgrade_downgrade,
 * ]
 */

import {withSkipRetryOnNetworkError} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {createChunks, getShardNames} from "jstests/sharding/libs/sharding_util.js";

const TestMode = {
    kSuccess: "success",
    kFailInAwaitingFetchTimestamp: "awaiting-fetch-timestamp",
    kFailInCreatingCollection: "creating-collection",
    kFailInCloning: "cloning",
    kFailInBuildingIndex: "building-index",
    kFailInApplying: "applying",
    kFailInStrictConsistency: "strict-consistency",
};

const TestModeOrdinal = {
    [TestMode.kFailInAwaitingFetchTimestamp]: 1,
    [TestMode.kFailInCreatingCollection]: 2,
    [TestMode.kFailInCloning]: 3,
    [TestMode.kFailInBuildingIndex]: 4,
    [TestMode.kFailInApplying]: 5,
    [TestMode.kFailInStrictConsistency]: 6,
    [TestMode.kSuccess]: 7,
};

function happensBefore(a, b) {
    return TestModeOrdinal[a] < TestModeOrdinal[b];
}

const MetricAvailability = {
    recipientTotalBytes: TestMode.kFailInCloning,
    recipientTotalDocs: TestMode.kFailInCloning,
    recipientTotalFetched: TestMode.kFailInCloning,
    copyDurationMs: TestMode.kFailInCloning,
    totalBytesCloned: TestMode.kFailInCloning,
    totalDocumentsCloned: TestMode.kFailInCloning,
    totalOplogsFetched: TestMode.kFailInCloning,
    maxRecipientIndexes: TestMode.kFailInCloning,
    numberOfIndexesDelta: TestMode.kFailInCloning,
    recipientTotalApplied: TestMode.kSuccess,
    applyDurationMs: TestMode.kSuccess,
    criticalSectionDurationMs: TestMode.kSuccess,
    totalOplogsApplied: TestMode.kSuccess,
};

function metricAvailable(metric, mode) {
    const availableAt = MetricAvailability[metric];
    if (availableAt === undefined) {
        return true;
    }
    return !happensBefore(mode, availableAt);
}

function assertMetricGtZero(metric, source, mode) {
    if (metricAvailable(metric, mode)) {
        assert.gt(source[metric], 0, metric);
    }
}

function assertMetricEq(metric, source, expected, mode) {
    if (metricAvailable(metric, mode)) {
        assert.eq(source[metric], expected, metric);
    }
}

const dbName = db.getName();
const oldShardKey = {
    oldKey: 1,
};
const newShardKey = {
    newKey: 1,
};

main();

function main() {
    const testCases = [TestMode.kSuccess, TestMode.kFailInCloning, TestMode.kFailInAwaitingFetchTimestamp];

    for (const mode of testCases) {
        const isSuccess = mode === TestMode.kSuccess;
        jsTest.log.info(`Testing ${isSuccess ? "success" : `failure during ${mode} phase`}`);
        const collName = `${jsTestName()}-${mode}`;
        initializeCollection(collName);
        const uuid = UUID();
        const failpoints = isSuccess ? [] : setFailInPhase(mode);
        const result = runResharding(collName, uuid);
        if (isSuccess) {
            assert.commandWorked(result);
        } else {
            assert.commandFailed(result);
        }
        unsetFailpoints(failpoints);
        const logs = getCompletionLogs(uuid);
        verifyCompletionLogs(collName, logs, mode);
    }
}

function initializeCollection(collName) {
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
    assert.commandWorked(coll.insert(docs));
}

function runResharding(collName, uuid) {
    jsTest.log.info(`Running resharding with user supplied UUID: ${tojson(uuid)}`);
    return db.adminCommand({reshardCollection: `${dbName}.${collName}`, key: newShardKey, reshardingUUID: uuid});
}

function getAllReplicaSets() {
    const topology = DiscoverTopology.findConnectedNodes(db);
    const allReplicaSets = [];
    allReplicaSets.push(topology.configsvr.nodes);
    for (const shard of Object.values(topology.shards)) {
        allReplicaSets.push(shard.nodes);
    }
    return allReplicaSets.map((rs) => rs.map((host) => new Mongo(host)));
}

function setFailInPhase(phase) {
    const failpoints = [];
    for (const rs of getAllReplicaSets()) {
        failpoints.push(
            configureFailPointForRS(rs, "reshardingRecipientFailInPhase", {
                phase,
                errorMessage: "reshard_collection_atlas_log_ingestion.js",
            }),
        );
    }
    return failpoints;
}

function unsetFailpoints(failpoints) {
    for (const failpoint of failpoints) {
        failpoint.off();
    }
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
                jsTest.log.error(`Failed to fetch logs: ${tojson(e)}`);
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
    return logs
        .filter((log) => log.includes(`"id":7763800,`))
        .filter((log) => {
            return log.includes(extractUUIDFromObject(uuid));
        });
}

function verifyCompletionLogs(collName, logs, mode) {
    // There could be multiple completion logs if deleting the coordinator state document is rolled
    // back.
    assert.gt(logs.length, 0, "No log lines with id 7763800 emitted on config server following resharding.");
    const completionLog = JSON.parse(logs.pop());
    jsTest.log.info(`Completion log found: ${tojson(completionLog)}`);
    const info = completionLog.attr.info;
    const stats = info.statistics;
    const isSuccess = mode === TestMode.kSuccess;
    const expectedStatus = isSuccess ? "success" : "failed";
    assert.eq(info.status, expectedStatus, "status");
    assert(info.hasOwnProperty("userSuppliedUUID"), "Missing userSuppliedUUID");
    if (!isSuccess) {
        assert(info.hasOwnProperty("failureReason"), "Missing failureReason");
    }
    assert.eq(stats.ns, `${dbName}.${collName}`, "namespace");
    assert.gt(stats.operationDurationMs, 0, "operationDurationMs");
    assert.eq(stats.provenance, "reshardCollection");
    assert.gt(stats.numberOfSourceShards, 0, "numberOfSourceShards");
    assert.gt(stats.numberOfDestinationShards, 0, "numberOfDestinationShards");
    verifyDonorMetrics(stats, mode);
    verifyRecipientMetrics(stats, mode);
    verifyTotals(stats, mode);
    verifyCriticalSection(stats, mode);
}

function verifyDonorMetrics(stats, mode) {
    assert(stats.hasOwnProperty("donors"), "Missing donors");
    assert.gt(Object.keys(stats.donors).length, 0, "No donors reported");
    const isSuccess = mode === TestMode.kSuccess;
    for (const donor of Object.values(stats.donors)) {
        assert(donor.hasOwnProperty("shardName"), "Missing donor shardName");
        assert.gt(donor.bytesToClone, 0, "bytesToClone");
        assert.gt(donor.documentsToClone, 0, "documentsToClone");
        assert.gt(donor.indexCount, 0, "donor indexCount");
        assert(donor.hasOwnProperty("writesDuringCriticalSection"), "Missing writesDuringCriticalSection");
        if (isSuccess) {
            assert(donor.hasOwnProperty("phaseDurations"), "Missing donor phaseDurations");
            assert.gte(donor.phaseDurations.criticalSectionDurationMs, 0, "criticalSectionDurationMs");
            assert(donor.hasOwnProperty("criticalSectionInterval"));
            const interval = donor.criticalSectionInterval;
            assert(interval.hasOwnProperty("start"), "Missing criticalSectionInterval start");
            assert(interval.hasOwnProperty("stop"), "Missing criticalSectionInterval stop");
        }
    }
}

function verifyRecipientMetrics(stats, mode) {
    assert(stats.hasOwnProperty("recipients"), "Missing recipients");
    assert.gt(Object.keys(stats.recipients).length, 0, "No recipients reported");
    const isSuccess = mode === TestMode.kSuccess;
    const recipientTotals = {
        recipientTotalBytes: 0,
        recipientTotalDocs: 0,
        recipientTotalFetched: 0,
        recipientTotalApplied: 0,
    };
    for (const recipient of Object.values(stats.recipients)) {
        assert(recipient.hasOwnProperty("shardName"), "Missing recipient shardName");
        recipientTotals.recipientTotalBytes += recipient.bytesCloned;
        recipientTotals.recipientTotalDocs += recipient.documentsCloned;
        recipientTotals.recipientTotalFetched += recipient.oplogsFetched;
        recipientTotals.recipientTotalApplied += recipient.oplogsApplied;
        assert(recipient.hasOwnProperty("phaseDurations"), "Missing recipient phaseDurations");
        for (const [phaseName, duration] of Object.entries(recipient.phaseDurations)) {
            assert.gt(duration, 0, `Phase ${phaseName} had no duration`);
        }
        if (isSuccess) {
            for (const expected of ["copyDurationMs", "applyDurationMs", "buildingIndexDurationMs"]) {
                assert(
                    recipient.phaseDurations.hasOwnProperty(expected),
                    `Successful operation did not report ${expected}`,
                );
            }
            assert.gt(recipient.indexCount, 0, "recipient indexCount");
        }
    }
    assertMetricGtZero("recipientTotalBytes", recipientTotals, mode);
    assertMetricGtZero("recipientTotalDocs", recipientTotals, mode);
    assertMetricGtZero("recipientTotalFetched", recipientTotals, mode);
    assertMetricGtZero("recipientTotalApplied", recipientTotals, mode);
}

function verifyTotals(stats, mode) {
    assert(stats.hasOwnProperty("totals"), "Missing totals");
    const totals = stats.totals;

    assertMetricGtZero("copyDurationMs", totals, mode);
    assertMetricGtZero("applyDurationMs", totals, mode);
    assertMetricGtZero("criticalSectionDurationMs", totals, mode);

    assert.gt(totals.totalBytesToClone, 0, "totalBytesToClone");
    assert.gt(totals.totalDocumentsToClone, 0, "totalDocumentsToClone");
    assert.gt(totals.averageDocSize, 0, "averageDocSize");

    assertMetricGtZero("totalBytesCloned", totals, mode);
    assertMetricGtZero("totalDocumentsCloned", totals, mode);
    assertMetricGtZero("totalOplogsFetched", totals, mode);
    assertMetricGtZero("totalOplogsApplied", totals, mode);

    assert.gt(totals.maxDonorIndexes, 0, "maxDonorIndexes");
    assertMetricGtZero("maxRecipientIndexes", totals, mode);
    assertMetricEq("numberOfIndexesDelta", totals, totals.maxRecipientIndexes - totals.maxDonorIndexes, mode);
}

function verifyCriticalSection(stats, mode) {
    const isSuccess = mode === TestMode.kSuccess;
    assert(stats.hasOwnProperty("criticalSection") === isSuccess, "Incorrect critical section presence");
    if (!isSuccess) {
        return;
    }
    const criticalSection = stats.criticalSection;
    assert(criticalSection.hasOwnProperty("interval"), "Missing interval");
    const interval = criticalSection.interval;
    assert(interval.hasOwnProperty("start"), "Missing criticalSection start");
    assert(interval.hasOwnProperty("stop"), "Missing criticalSection stop");
    assert(criticalSection.hasOwnProperty("expiration"), "Missing expiration");
    assert(
        criticalSection.hasOwnProperty("totalWritesDuringCriticalSection"),
        "Missing totalWritesDuringCriticalSection",
    );
}
