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
 *   5. Failover before persist: pause the scan just before it upserts the state doc, stepdown to
 *      interrupt it, disable the failpoint, and confirm the next primary re-runs the scan to
 *      completion.
 *   6. Multiple buggy zones on the same collection: the scan stops on the first match and records
 *      foundBuggyZone=true / alertEmitted=true.
 *   7. Mix of one buggy and several well-formed zones across different collections: the scan flags
 *      the cluster overall.
 *   8. Non-retryable catalog read error: failCommand fails the scan's config.tags aggregation with
 *      BadValue; the scan abandons the sweep and increments the maxKeyZoneScanErrors FTDC counter.
 *
 * @tags: [
 *  featureFlagMaxKeyDetection,
 *  requires_fcv_90,
 *  does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
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

function readZoneScanStats(rs) {
    return assert.commandWorked(rs.getPrimary().adminCommand({serverStatus: 1})).shardingStatistics;
}

// Asserts the FTDC counters reflect a completed scan with the given outcome. Cases wait on the state
// doc for completion (scanCompletedAt) and assert the classification here, so it is checked once.
function assertZoneScanStats(rs, {foundBuggyZone, alertEmitted}, message) {
    let stats;
    assert.soon(
        () => {
            stats = readZoneScanStats(rs);
            return (
                stats.maxKeyZoneScanComplete == 1 &&
                stats.maxKeyZoneScanFoundBuggyZone == (foundBuggyZone ? 1 : 0) &&
                stats.maxKeyZoneScanAlertEmitted == (alertEmitted ? 1 : 0)
            );
        },
        () => `${message}; got ${tojson(stats)}`,
    );
}

// Steps the config-server primary up and polls for the state doc to satisfy 'predicate'. The
// scan's persist can fail transiently with NotWritablePrimary in topologies that churn primary
// status during stepup; the outer try/catch in onStepUpComplete swallows it and leaves the
// state doc absent, so the test re-steps until the next stable primary completes the scan.
function stepUpAndAwaitScanState(rs, predicate, message, maxAttempts = 10) {
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

const dbName = jsTestName();
const mongos = st.s0;
const adminDB = mongos.getDB("admin");
const testDB = mongos.getDB(dbName);

assert.commandWorked(
    adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// --- Case 1: clean cluster with a well-formed compound zone tag ---------------------------------
jsTest.log.info("Case 1: clean cluster, expect foundBuggyZone=false after stepup");

const wellFormedColl = "wellFormedColl";
const wellFormedNs = `${dbName}.${wellFormedColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: wellFormedNs, key: {a: 1, b: 1}}));

assert.commandWorked(adminDB.runCommand({addShardToZone: st.shard0.shardName, zone: "zoneA"}));
assert.commandWorked(
    adminDB.runCommand({
        updateZoneKeyRange: wellFormedNs,
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
        updateZoneKeyRange: wellFormedNs,
        min: {a: 20, b: MinKey},
        max: {a: 30},
        zone: "zoneA",
    }),
);

// A sharded compound-key collection with no zones at all. It has no config.tags entries, so the
// config.tags/config.collections inner join must simply never surface it. Confirms the scan does
// not error on, or flag, untagged sharded collections.
const untaggedNs = `${dbName}.untaggedColl`;
assert.commandWorked(adminDB.runCommand({shardCollection: untaggedNs, key: {a: 1, b: 1}}));

// Clear the state doc the initial primary stepup may have already persisted (with empty
// config.tags) so the next stepup re-runs the scan and actually classifies the tags we
// just inserted.
clearScanStateAndWaitMajority(st.configRS);

const cleanState = stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan state document should appear with scanCompletedAt set on a cluster with only well-formed " +
        "and legitimate prefix zone tags",
);
assertZoneScanStats(
    st.configRS,
    {foundBuggyZone: false, alertEmitted: false},
    "maxKeyZoneScan stats must reflect clean scan outcome",
);

jsTest.log.info("Case 1 follow-up: second stepup must short-circuit");
stepDownAndUp(st.configRS);
// Wait for the scan's short-circuit log (this term) -- proof it ran and skipped -- then assert it
// left the doc untouched.
const guardTerm = assert.commandWorked(
    st.configRS.getPrimary().adminCommand({replSetGetStatus: 1}),
).term;
checkLog.containsJson(st.configRS.getPrimary(), 12829503, {term: Number(guardTerm)});
assert.docEq(
    cleanState,
    readScanState(st.configRS),
    "State doc must be unchanged after re-stepup once scanCompletedAt is persisted",
);
// The short-circuit path re-publishes the prior outcome to FTDC (distinct from the initial publish).
assertZoneScanStats(
    st.configRS,
    {foundBuggyZone: false, alertEmitted: false},
    "short-circuit must republish the prior clean outcome to FTDC stats",
);

// --- Case 2: buggy MinKey fingerprint on a compound shard key -----------------------------------
jsTest.log.info("Case 2: direct-injected buggy tag on a compound shard key");

const buggyColl = "buggyColl";
const buggyNs = `${dbName}.${buggyColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: buggyNs, key: {a: 1, b: 1}}));

insertTagDirectly(
    st.configRS,
    buggyNs,
    "buggyZone",
    {a: MinKey, b: MinKey},
    {a: MaxKey, b: MinKey},
);

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan should complete after a buggy MinKey fingerprint is injected",
);
assertZoneScanStats(
    st.configRS,
    {foundBuggyZone: true, alertEmitted: true},
    "maxKeyZoneScan stats must reflect buggy zone detection outcome",
);

assert.soon(
    () => rawMongoProgramOutput('"id":12829504').length > 0,
    "Expected WARNING log id 12829504 to be emitted when the buggy MinKey fingerprint is detected",
);

// --- Case 3: single-field shard key is exempt --------------------------------------------------
// Drop Case 2's collection and remove its tag to isolate the single-field case.
jsTest.log.info("Case 3: single-field shard key is exempt from the MaxKey zone scan");

assert.commandWorked(testDB.runCommand({drop: buggyColl}));
assert.commandWorked(
    st.configRS
        .getPrimary()
        .getDB("config")
        .getCollection("tags")
        .remove({ns: buggyNs}, {writeConcern: {w: "majority"}}),
);

const singleFieldColl = "singleFieldColl";
const singleFieldNs = `${dbName}.${singleFieldColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: singleFieldNs, key: {a: 1}}));

// A valid full-range zone on a single-field shard key. The scan skips single-field collections
// before ever inspecting their zones, so this must not be flagged.
assert.commandWorked(
    adminDB.runCommand({
        updateZoneKeyRange: singleFieldNs,
        min: {a: MinKey},
        max: {a: MaxKey},
        zone: "zoneA",
    }),
);

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan should complete on a cluster whose only tag is on a single-field (exempt) shard key",
);
assertZoneScanStats(
    st.configRS,
    {foundBuggyZone: false, alertEmitted: false},
    "Scan must skip single-field shard keys and report foundBuggyZone=false",
);

// --- Case 4: aggregation cursor invalidation is retried ----------------------------------------
jsTest.log.info("Case 4: scan retries the config.tags aggregation past QueryPlanKilled");

// Fail the scan's config.tags aggregation with QueryPlanKilled fewer times than the retry budget
// (defaultClientMaxRetryAttempts, default 3) so the scan retries and still completes. The scan runs
// on the Balancer thread (no client session), so both failLocalClients and failInternalCommands are
// needed for failCommand to intercept it.
configureFailCommandAllConfigNodes(st.configRS, {
    configureFailPoint: "failCommand",
    mode: {times: 2},
    data: {
        failCommands: ["aggregate"],
        namespace: "config.tags",
        errorCode: ErrorCodes.QueryPlanKilled,
        failLocalClients: true,
        failInternalCommands: true,
    },
});

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan should retry the aggregation past injected QueryPlanKilled errors and still complete",
);
assertZoneScanStats(
    st.configRS,
    {foundBuggyZone: false, alertEmitted: false},
    "Scan that retried past QueryPlanKilled must still report the clean outcome",
);

configureFailCommandAllConfigNodes(st.configRS, {configureFailPoint: "failCommand", mode: "off"});

// --- Case 5: stepdown while the scan is paused just before persisting ---------------------------
// Arm the failpoint on the next primary so its scan parks just before the upsert, then stepdown to
// interrupt the parked scan (pauseWhileSet honours the opCtx interrupt from killOpForStepdown). The
// next primary must re-run the scan to completion.
jsTest.log.info("Case 5: stepdown while the scan is paused mid-flight");

// Recreate buggyColl with a buggy zone so the resumed scan has something to iterate over.
assert.commandWorked(adminDB.runCommand({shardCollection: buggyNs, key: {a: 1, b: 1}}));
insertTagDirectly(
    st.configRS,
    buggyNs,
    "buggyZone",
    {a: MinKey, b: MinKey},
    {a: MaxKey, b: MinKey},
);

clearScanStateAndWaitMajority(st.configRS);

const failoverNextPrimary = st.configRS.getSecondary();
const failoverFp = configureFailPoint(
    failoverNextPrimary,
    "hangBeforePersistingMaxKeyZoneScanState",
);

st.configRS.stepUp(failoverNextPrimary);
st.configRS.waitForPrimary();
failoverFp.wait({maxTimeMS: kDefaultWaitForFailPointTimeout});

stepDownAndUp(st.configRS);

// Only failoverNextPrimary was armed, so disable via the handle.
failoverFp.off();

// Clear any doc written meanwhile so the next stepup re-runs from an absent doc.
clearScanStateAndWaitMajority(st.configRS);

// Only require that scanCompletedAt is persisted; whether the buggy zone is observed depends on how
// far the interrupted scan had iterated.
stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Expected the next primary to re-run the zone scan to completion after the in-flight scan was " +
        "interrupted by stepdown",
);

// --- Case 6: multiple buggy zones on the same compound-key collection ---------------------------
// The scan early-breaks on the first match and persists foundBuggyZone=true. Clean up Case 5's
// fixtures first so the assertion is unambiguous.
jsTest.log.info("Case 6: multiple buggy zones on the same compound-key collection");

assert.commandWorked(testDB.runCommand({drop: buggyColl}));
assert.commandWorked(
    st.configRS
        .getPrimary()
        .getDB("config")
        .getCollection("tags")
        .remove({}, {writeConcern: {w: "majority"}}),
);

const multiBuggyColl = "multiBuggyColl";
const multiBuggyNs = `${dbName}.${multiBuggyColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: multiBuggyNs, key: {a: 1, b: 1}}));

insertTagDirectly(
    st.configRS,
    multiBuggyNs,
    "buggyZoneA",
    {a: MinKey, b: MinKey},
    {a: MaxKey, b: MinKey},
);
insertTagDirectly(
    st.configRS,
    multiBuggyNs,
    "buggyZoneB",
    {a: 100, b: MinKey},
    {a: MaxKey, b: MinKey},
);

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan should complete on a collection carrying multiple buggy zones",
);
assertZoneScanStats(
    st.configRS,
    {foundBuggyZone: true, alertEmitted: true},
    "Scan must flag the first matching buggy zone among several on one collection",
);

// --- Case 7: one buggy zone alongside several well-formed zones across collections --------------
jsTest.log.info("Case 7: cluster with one buggy zone among many well-formed zones is flagged");

assert.commandWorked(testDB.runCommand({drop: multiBuggyColl}));
assert.commandWorked(
    st.configRS
        .getPrimary()
        .getDB("config")
        .getCollection("tags")
        .remove({}, {writeConcern: {w: "majority"}}),
);

const normalRangeColl = "normalRangeColl";
const normalRangeNs = `${dbName}.${normalRangeColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: normalRangeNs, key: {a: 1, b: 1}}));
assert.commandWorked(
    adminDB.runCommand({
        updateZoneKeyRange: normalRangeNs,
        min: {a: 0, b: MinKey},
        max: {a: 100, b: MaxKey},
        zone: "zoneA",
    }),
);

const fullRangeColl = "fullRangeColl";
const fullRangeNs = `${dbName}.${fullRangeColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: fullRangeNs, key: {a: 1, b: 1}}));
assert.commandWorked(
    adminDB.runCommand({
        updateZoneKeyRange: fullRangeNs,
        min: {a: MinKey, b: MinKey},
        max: {a: MaxKey, b: MaxKey},
        zone: "zoneA",
    }),
);

const mixedBuggyColl = "mixedBuggyColl";
const mixedBuggyNs = `${dbName}.${mixedBuggyColl}`;
assert.commandWorked(adminDB.runCommand({shardCollection: mixedBuggyNs, key: {a: 1, b: 1}}));
insertTagDirectly(
    st.configRS,
    mixedBuggyNs,
    "buggyMixed",
    {a: MinKey, b: MinKey},
    {a: MaxKey, b: MinKey},
);

clearScanStateAndWaitMajority(st.configRS);

stepUpAndAwaitScanState(
    st.configRS,
    (doc) => doc.scanCompletedAt !== undefined,
    "Scan should complete on a cluster mixing one buggy zone with well-formed zones",
);
assertZoneScanStats(
    st.configRS,
    {foundBuggyZone: true, alertEmitted: true},
    "Scan must flag the cluster when at least one buggy zone exists alongside well-formed zones",
);

// --- Case 8: a non-retryable catalog read error increments maxKeyZoneScanErrors ----------------
// BadValue is non-fatal (not rethrown) and non-retryable, so it reaches the scan's catch block: the
// sweep is abandoned (no scanCompletedAt) and maxKeyZoneScanErrors is bumped. See Case 4 for why
// both failLocalClients and failInternalCommands are required.
jsTest.log.info("Case 8: non-retryable catalog read error increments maxKeyZoneScanErrors");

clearScanStateAndWaitMajority(st.configRS);

configureFailCommandAllConfigNodes(st.configRS, {
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["aggregate"],
        namespace: "config.tags",
        errorCode: ErrorCodes.BadValue,
        failLocalClients: true,
        failInternalCommands: true,
    },
});

stepDownAndUp(st.configRS);

// failCommand is armed on every config node, so whichever node the scan lands on bumps the counter.
let erroredZoneStats;
assert.soon(
    () => {
        erroredZoneStats = readZoneScanStats(st.configRS);
        return erroredZoneStats.maxKeyZoneScanErrors >= 1;
    },
    () =>
        `maxKeyZoneScanErrors must increment when the scan hits a non-retryable catalog read error; ` +
        `got ${tojson(erroredZoneStats)}`,
);

// Corroborate independently of the new counter: an abandoned scan upserts a doc but omits
// scanCompletedAt.
let erroredDoc;
assert.soon(() => {
    erroredDoc = readScanState(st.configRS);
    return erroredDoc !== null;
}, "Scan should still upsert a state doc when the sweep is abandoned");
assert.eq(
    undefined,
    erroredDoc.scanCompletedAt,
    "An abandoned scan must not persist scanCompletedAt",
    {erroredDoc},
);

configureFailCommandAllConfigNodes(st.configRS, {configureFailPoint: "failCommand", mode: "off"});

st.stop();
