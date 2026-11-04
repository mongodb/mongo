/**
 * Tests that a unique primary-driven index build that fails over during its load phase will not
 * commit duplicate keys when the resume point falls between two equal keys. The resumed load should
 * compare its first key with the last key loaded before the failover, rejecting/failing on the
 * duplicate. No nodes should end up with a ready unique index that holds both entries, and the
 * internode dbHash check should pass. The build should fail with DuplicateKey error.
 *
 * Server parameters save a resume point after every key, and a fail point holds the load between
 * the two equal keys, where an ill-timed failover would.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    PdibFailoverMode,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// The test kills the primary. Let the shell clean up any core dump it produces.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

// A 1 MB sorter budget makes the collection scan spill, so the resume takes the bulk load branch.
const INDEX_BUILD_MEM_MB = 1;
// Filler keys are distinct and sort before the duplicate value. Their total size forces a spill.
const N_FILLER = 200;
const KEY_PAD = 8000;
const DUP_VALUE = "m-dup";
// Pause the load after the first duplicate key is committed and before the second one is read.
const PAUSE_ITERATION = N_FILLER + 1;
const INDEX_NAME = "a_1";
const TOTAL_DOCS = N_FILLER + 2;

const dbName = "test";
const collName = "pdib_dup_straddle";
const ctrlCollName = "pdib_dup_straddle_ctrl";

function makeFiller(i) {
    // Sorts before DUP_VALUE and embeds i so every key is distinct.
    return {_id: i, a: "a" + String(i).padStart(7, "0") + "x".repeat(KEY_PAD)};
}

function insertData(coll) {
    const BATCH = 50;
    for (let start = 0; start < N_FILLER; start += BATCH) {
        const docs = [];
        for (let i = start; i < Math.min(start + BATCH, N_FILLER); i++) {
            docs.push(makeFiller(i));
        }
        assert.commandWorked(coll.insert(docs));
    }
    assert.commandWorked(coll.insert({_id: N_FILLER, a: DUP_VALUE}));
    assert.commandWorked(coll.insert({_id: N_FILLER + 1, a: DUP_VALUE}));
}

function validateForDup(coll, label) {
    const res = assert.commandWorked(coll.validate({full: true}));
    const keys = res.keysPerIndex ? res.keysPerIndex[INDEX_NAME] : undefined;
    jsTest.log.info(
        `validate(${label}): valid=${res.valid} keysPerIndex.${INDEX_NAME}=${keys} ` +
            `errors=${tojson(res.errors)}`,
    );
    const details = res.indexDetails ? res.indexDetails[INDEX_NAME] : undefined;
    if (details && details.errors && details.errors.length) {
        jsTest.log.error(`validate(${label}) ${INDEX_NAME} errors: ${tojson(details.errors)}`);
    }
    return res;
}

// Asserts that the resumed build failed due to a DuplicateKey error.
function assertBuildAbortedWithDuplicateKey(rst, node, label) {
    const coll = node.getDB(dbName)[collName];
    IndexBuildTest.waitForIndexBuildToStop(node.getDB(dbName), collName, INDEX_NAME);

    assert.soon(
        () => !coll.getIndexes({includeBuildUUIDs: true}).some((i) => i.name === INDEX_NAME),
        `${label}: ${INDEX_NAME} must not exist as ready or in-progress after the index build aborts`,
    );

    // Ensure that the duplicate-key docs are there, but the index is not.
    assert.eq(
        2,
        coll.find({a: DUP_VALUE}).itcount(),
        `${label}: both duplicate key docs must remain`,
    );
    IndexBuildTest.assertIndexes(coll, 1, ["_id_"]);

    const cmdNs = node.getDB(dbName).getCollection("$cmd").getFullName();
    let ops;
    assert.soon(() => {
        ops = rst.dumpOplog(node, {
            op: "c",
            ns: cmdNs,
            "o.abortIndexBuild": collName,
            "o.indexes.name": INDEX_NAME,
        });
        return ops.length > 0;
    }, `${label}: no abortIndexBuild oplog entry appeared`);
    assert.eq(1, ops.length, `${label}: expected exactly one abortIndexBuild entry`, {ops});
    assert.eq(
        ErrorCodes.DuplicateKey,
        ops[0].o.cause.code,
        `${label}: expected build to abort with DuplicateKey`,
        {cause: ops[0].o.cause},
    );
}

const rst = PrimaryDrivenResumableIndexBuildTest.setUp({
    nodes: 3,
    nodeParams: {
        // Save the resume point and commit a batch after every key.
        primaryDrivenIndexBuildLoadResumeStateWriteIntervalKeys: 1,
        primaryDrivenIndexBuildIndexInsertionBatchSize: 1,
        // Keep the pool block below the build budget so the scan spills only a few ranges.
        operationMemoryPoolBlockMaxSizeKB: 256,
        // Disable periodic checkpoints as they are manually forced in the test's failover.
        syncdelay: 0,
    },
});

if (
    !PrimaryDrivenResumableIndexBuildTest.preflightSetup(rst, dbName, jsTestName(), {
        // Startup enforces a 50 MB minimum for this parameter, so set the small budget at runtime.
        maxIndexBuildMemoryUsageMegabytes: INDEX_BUILD_MEM_MB,
    })
) {
    jsTest.log.info("Skipping test due to preflight checks.");
    PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
    quit();
}

const primary = rst.getPrimary();
rst.getSecondaries().forEach((s) => {
    s.setSecondaryOk();
});
rst.waitForMyState(primary, ReplSetTest.State.PRIMARY);

const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];

jsTest.log.info(
    "CONTROL: build the unique index on the SAME data with no failover; must fail DuplicateKey.",
);
assert.commandWorked(primaryDB.createCollection(ctrlCollName));
insertData(primaryDB[ctrlCollName]);
assert.eq(TOTAL_DOCS, primaryDB[ctrlCollName].countDocuments({}));
const ctrlRes = primaryDB.runCommand({
    createIndexes: ctrlCollName,
    indexes: [{key: {a: 1}, name: INDEX_NAME, unique: true}],
});
jsTest.log.info(`CONTROL: createIndexes result = ${tojson(ctrlRes)}`);
assert.commandFailedWithCode(
    ctrlRes,
    ErrorCodes.DuplicateKey,
    "CONTROL: a unique build on this exact data (no failover) must be rejected with DuplicateKey",
);
jsTest.log.info("CONTROL passed: the data pattern is normally rejected.");

jsTest.log.info(`Seeding ${TOTAL_DOCS} docs (${N_FILLER} distinct fillers + 2 equal-key docs).`);
assert.commandWorked(primaryDB.createCollection(collName));
insertData(primaryColl);
assert.eq(TOTAL_DOCS, primaryColl.countDocuments({}));
rst.awaitReplication();
rst.getSecondaries().forEach((s) => {
    assert.eq(
        TOTAL_DOCS,
        s.getDB(dbName)[collName].countDocuments({}),
        `${s} did not replicate the seed documents`,
    );
});

jsTest.log.info(
    `Arming hangIndexBuildDuringBulkLoadPhase{iteration:${PAUSE_ITERATION}} and starting ` +
        `createIndexes({a:1}, unique:true).`,
);
const loadHang = configureFailPoint(primary, "hangIndexBuildDuringBulkLoadPhase", {
    iteration: NumberLong(PAUSE_ITERATION),
    indexNames: [INDEX_NAME],
});
const awaitBuild = IndexBuildTest.startIndexBuild(
    primary,
    primaryColl.getFullName(),
    {a: 1},
    {name: INDEX_NAME, unique: true},
    [
        ErrorCodes.InterruptedDueToReplStateChange,
        ErrorCodes.IndexBuildAborted,
        ErrorCodes.InterruptedAtShutdown,
    ],
);

jsTest.log.info("Waiting for the load to pause at the straddle (log 4924400).");
loadHang.wait();

jsTest.log.info(
    "Waiting for the replicated load state (through the FIRST duplicate key) to reach secondaries.",
);
rst.awaitReplication();

jsTest.log.info("SIGKILL the primary and failover while the load is paused at the straddle.");
const oldPrimaryNodeId = rst.getNodeId(primary);
const newPrimary = PrimaryDrivenResumableIndexBuildTest.failover(
    rst,
    PdibFailoverMode.UNCLEAN_RESTART,
);

try {
    awaitBuild();
} catch (e) {
    jsTest.log.info(
        `Ignoring index-build shell failure after primary kill (expected): ${tojson(e)}`,
    );
}

// The build must resume in the bulk load phase. A resume that rescans the collection will always
// find the duplicate. If the build resumes in the 'collection scan' phase, the scan did not spill
// and N_FILLER/KEY_PAD should be increased.
jsTest.log.info(
    "Confirming the resume took the 'bulk load' branch on the new primary (log 12500800).",
);
checkLog.containsJson(newPrimary, 12500800, {phase: "bulk load"});

jsTest.log.info("Asserting the build for the unique index did not complete on NEW primary.");
assertBuildAbortedWithDuplicateKey(rst, newPrimary, "new primary");

jsTest.log.info("Checking the former primary, now secondary.");
const oldPrimary = rst.nodes[oldPrimaryNodeId];
oldPrimary.setSecondaryOk();
rst.awaitSecondaryNodes(/* timeout */ undefined, [oldPrimary]);
rst.awaitReplication();

jsTest.log.info("Asserting no corrupt index on restarted former primary.");
// Skip the insert check because the node is a secondary.
{
    const coll = oldPrimary.getDB(dbName)[collName];
    assertBuildAbortedWithDuplicateKey(rst, oldPrimary, "restarted former primary");
    const res = validateForDup(coll, "restarted former primary");
    assert(res.valid, `restarted former primary: must be VALID: ${tojson(res)}`);
}

// The test already ran stricter validation than teardown's default validation, so skip it.
PrimaryDrivenResumableIndexBuildTest.tearDown(rst, {
    skipValidation: true,
});
