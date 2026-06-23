/**
 * MaxKey zone one-shot inventory scan in the Balancer.
 *
 * Steps the config-server primary up against a 2-shard cluster and exercises these cases:
 *   1. Clean cluster: a well-formed compound zone tag must classify as not buggy, and a sharded
 *      collection with no zones at all must be ignored by the config.tags/config.collections join. A
 *      second stepup must not rewrite the state doc (one-shot guard).
 *   2. Buggy fingerprint: a tag whose upper bound is {a: MaxKey, b: MinKey} on a
 *      compound shard key {a: 1, b: 1}. This is the shape an all-MaxKey ("global max")
 *      prefix would have produced if it had been extended to the full shard key with a
 *      trailing MinKey instead of MaxKey. Direct-insert into config.tags bypasses
 *      validation in addShardToZone/updateZoneKeyRange. The scan must flag it.
 *   3. Single-field shard key: a full-range zone on a single-field shard key {a: 1}.
 *      Single-field shard keys are exempt (the buggy fingerprint requires a compound
 *      key), so the scan must not flag it.
 *   4. Aggregation cursor invalidation: the scan's config.tags aggregation is failed with
 *      QueryPlanKilled a bounded number of times via failCommand; the scan must retry and still
 *      complete within a single stepup.
 *
 * @tags: [
 *  featureFlagMaxKeyDetection,
 *  requires_fcv_90,
 *  does_not_support_stepdowns,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const scanStateId = "scanState";

function readScanState(rs) {
    return rs.getPrimary().getDB("config").getCollection("maxKeyZoneScanState").findOne({
        _id: scanStateId,
    });
}

function stepDownAndUp(rs) {
    const newPrimary = rs.getSecondary();
    rs.stepUp(newPrimary);
    rs.waitForPrimary();
}

// Steps the config-server primary up and polls for the state doc to satisfy 'predicate'. The
// scan's persist can fail transiently with NotWritablePrimary in topologies that churn primary
// status during stepup; the outer try/catch in onStepUpComplete swallows it and leaves the
// state doc absent, so the test re-steps until the next stable primary completes the scan.
function stepUpAndAwaitScanState(rs, predicate, message, maxAttempts = 4) {
    let last = null;
    for (let attempt = 1; attempt <= maxAttempts; attempt++) {
        stepDownAndUp(rs);
        const deadline = Date.now() + 15 * 1000;
        while (Date.now() < deadline) {
            last = readScanState(rs);
            if (last !== null) break;
            sleep(500);
        }
        if (last !== null) {
            assert(predicate(last), `${message}; got: ${tojson(last)}`);
            return last;
        }
        jsTest.log.info("Scan state doc not observed; retrying", {attempt, maxAttempts});
    }
    assert(false, `${message}; state doc never appeared after ${maxAttempts} stepups`);
}

// The scan is one-shot: once any primary persists scanCompletedAt, subsequent stepups
// short-circuit. Cases 2 and 3 need a fresh enumeration, so they delete the state doc with
// majority write concern before the next stepup so whichever node is promoted observes it
// absent.
function clearScanStateAndWaitMajority(rs) {
    const primary = rs.getPrimary();
    assert.commandWorked(
        primary.getDB("config").runCommand({
            delete: "maxKeyZoneScanState",
            deletes: [{q: {_id: scanStateId}, limit: 0}],
            writeConcern: {w: "majority"},
        }),
    );
    rs.awaitReplication();
}

// Applies a failCommand configuration to every config-server node, since a stepup can promote any
// of them and the scan runs on whichever node becomes primary.
function configureFailCommandAllConfigNodes(rs, cmd) {
    rs.nodes.forEach((node) => {
        assert.commandWorked(node.adminCommand(cmd));
    });
}

function insertTagDirectly(rs, ns, tagName, minBound, maxBound) {
    const primary = rs.getPrimary();
    assert.commandWorked(
        primary
            .getDB("config")
            .getCollection("tags")
            .insert(
                {_id: {ns, min: minBound}, ns, tag: tagName, min: minBound, max: maxBound},
                {writeConcern: {w: "majority"}},
            ),
    );
    rs.awaitReplication();
}

const st = new ShardingTest({shards: 2, configShard: false, config: 2});

const dbName = "testDb";
const mongos = st.s0;
const adminDB = mongos.getDB("admin");
const testDB = mongos.getDB(dbName);

assert.commandWorked(
    adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// --- Case 1: clean cluster with a well-formed compound zone tag ---------------------------------
jsTest.log.info("Case 1: clean cluster, expect foundBuggyZone=false after stepup");

const coll1 = "coll1";
const ns1 = `${dbName}.${coll1}`;
assert.commandWorked(adminDB.runCommand({shardCollection: ns1, key: {a: 1, b: 1}}));

assert.commandWorked(adminDB.runCommand({addShardToZone: st.shard0.shardName, zone: "zoneA"}));
assert.commandWorked(
    adminDB.runCommand({
        updateZoneKeyRange: ns1,
        min: {a: 0, b: MinKey},
        max: {a: 10, b: MaxKey},
        zone: "zoneA",
    }),
);
// A legitimate prefix-bounded zone: updateZoneKeyRange pads the missing trailing field with
// MinKey, so this is stored as max: {a: 30, b: MinKey}. This is byte-shaped like a "trailing
// MinKey" but is NOT the buggy fingerprint (a=30 is a normal value, not MaxKey), so the scan
// must not flag it. Guards against false positives on healthy clusters that use prefix zones.
assert.commandWorked(
    adminDB.runCommand({
        updateZoneKeyRange: ns1,
        min: {a: 20, b: MinKey},
        max: {a: 30},
        zone: "zoneA",
    }),
);

// A sharded compound-key collection with no zones at all. It has no config.tags entries, so the
// config.tags/config.collections inner join must simply never surface it. Confirms the scan does
// not error on, or flag, untagged sharded collections.
const ns1Untagged = `${dbName}.coll1Untagged`;
assert.commandWorked(adminDB.runCommand({shardCollection: ns1Untagged, key: {a: 1, b: 1}}));

// Clear the state doc the initial primary stepup may have already persisted (with empty
// config.tags) so the next stepup re-runs the scan and actually classifies the tags we
// just inserted.
clearScanStateAndWaitMajority(st.configRS);

const cleanState = stepUpAndAwaitScanState(
    st.configRS,
    (doc) =>
        doc.scanCompletedAt !== undefined &&
        doc.foundBuggyZone === false &&
        doc.alertEmitted === false,
    "Scan state document should appear with scanCompletedAt set, foundBuggyZone=false, " +
        "alertEmitted=false on a cluster with only well-formed and legitimate prefix zone tags",
);

jsTest.log.info("Case 1 follow-up: second stepup must short-circuit");
stepDownAndUp(st.configRS);
sleep(2 * 1000);
assert.docEq(
    cleanState,
    readScanState(st.configRS),
    "State doc must be unchanged after re-stepup once scanCompletedAt is persisted",
);

// --- Case 2: buggy MinKey fingerprint on a compound shard key -----------------------------------
jsTest.log.info("Case 2: direct-injected buggy tag on a compound shard key");

const coll2 = "coll2";
const ns2 = `${dbName}.${coll2}`;
assert.commandWorked(adminDB.runCommand({shardCollection: ns2, key: {a: 1, b: 1}}));

insertTagDirectly(st.configRS, ns2, "buggyZone", {a: MinKey, b: MinKey}, {a: MaxKey, b: MinKey});

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) =>
        doc.scanCompletedAt !== undefined &&
        doc.foundBuggyZone === true &&
        doc.alertEmitted === true,
    "Scan should flag the buggy MinKey fingerprint and persist alertEmitted=true on the first " +
        "foundBuggyZone transition",
);

assert.soon(
    () => rawMongoProgramOutput('"id":12829504').length > 0,
    "Expected WARNING log id 12829504 to be emitted when the buggy MinKey fingerprint is detected",
);

// --- Case 3: single-field shard key is exempt --------------------------------------------------
// Drop Case 2's collection and remove its tag to isolate the single-field case.
jsTest.log.info("Case 3: single-field shard key is exempt from the MaxKey zone scan");

assert.commandWorked(testDB.runCommand({drop: coll2}));
assert.commandWorked(
    st.configRS
        .getPrimary()
        .getDB("config")
        .getCollection("tags")
        .remove({ns: ns2}, {writeConcern: {w: "majority"}}),
);

const coll3 = "coll3";
const ns3 = `${dbName}.${coll3}`;
assert.commandWorked(adminDB.runCommand({shardCollection: ns3, key: {a: 1}}));

// A valid full-range zone on a single-field shard key. The scan skips single-field collections
// before ever inspecting their zones, so this must not be flagged.
assert.commandWorked(
    adminDB.runCommand({
        updateZoneKeyRange: ns3,
        min: {a: MinKey},
        max: {a: MaxKey},
        zone: "zoneA",
    }),
);

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) =>
        doc.scanCompletedAt !== undefined &&
        doc.foundBuggyZone === false &&
        doc.alertEmitted === false,
    "Scan should skip single-field shard keys and persist foundBuggyZone=false",
);

// --- Case 4: aggregation cursor invalidation is retried ----------------------------------------
jsTest.log.info("Case 4: scan retries the config.tags aggregation past QueryPlanKilled");

// Fail the scan's config.tags aggregation with QueryPlanKilled fewer times than the config shard's
// retry budget (defaultClientMaxRetryAttempts, default 3), so a single stepup's scan must retry via
// Shard::RetryPolicy::kIdempotentOrCursorInvalidated and still complete.
// failInternalCommands is required because the balancer issues the aggregation from an internal
// client; the namespace filter isolates the failure to the scan's own aggregation.
configureFailCommandAllConfigNodes(st.configRS, {
    configureFailPoint: "failCommand",
    mode: {times: 2},
    data: {
        failCommands: ["aggregate"],
        namespace: "config.tags",
        errorCode: ErrorCodes.QueryPlanKilled,
        failInternalCommands: true,
    },
});

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan should retry the aggregation past injected QueryPlanKilled errors and still complete",
);

configureFailCommandAllConfigNodes(st.configRS, {configureFailPoint: "failCommand", mode: "off"});

st.stop();
