/**
 * SERVER-115116: pins the per-build log line emitted at startup when an unfinished two-phase index
 * build is restarted instead of resumed. Asserts that log 11511160 surfaces a `reason` attribute
 * for each of the documented enum values, so operators can grep by reason and by buildUUID.
 *
 * Reasons exercised here:
 *  - parse_failed         via failpoint `failToParseResumeIndexInfo`
 *  - temp_files_missing   via removing `_tmp/` before restart
 *  - standalone_mode      via starting the node in standalone after a clean shutdown
 *  - unclean_shutdown     via SIGKILL
 *
 * `resume_setup_failed` is already pinned by restart_index_build_if_resume_fails.js (log 4841701);
 * `no_resume_ident` and `missing_storage_ident` are covered by unit tests in
 * catalog_repair_test.cpp and resumable_index_builds_common_test.cpp respectively.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable; the new log line only fires on the two-phase
 *   # restart path during startup recovery.
 *   primary_driven_index_builds_incompatible,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const kLogId = 11511160;
const dbName = "test";
const collName = jsTestName();
const indexSpec = {a: 1};

function assertReasonLogged(primary, buildUUID, reason) {
    checkLog.containsJson(primary, kLogId, {
        buildUUID: function (uuid) {
            return uuid && uuid["uuid"]["$uuid"] === buildUUID;
        },
        reason: reason,
    });
}

// ---------------------------------------------------------------------------
// Case 1: parse_failed — resume info doc is unparseable.
// ---------------------------------------------------------------------------
{
    jsTestLog("Case 1: reason=parse_failed");
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const coll = primary.getDB(dbName).getCollection(collName);
    assert.commandWorked(coll.insert({a: 1}));

    ResumableIndexBuildTest.runFailToResume(
        rst,
        dbName,
        collName,
        indexSpec,
        {failPointAfterStartup: "failToParseResumeIndexInfo"},
        [{a: 2}, {a: 3}],
        [{a: 4}, {a: 5}],
        true /* failWhileParsing */,
    );

    // Existing pre-restart parse-fail log is unchanged.
    checkLog.containsJson(primary, 4916300);
    // New per-build line with reason.
    checkLog.containsJson(primary, kLogId, {reason: "parse_failed"});

    rst.stopSet();
}

// ---------------------------------------------------------------------------
// Case 2: temp_files_missing — `_tmp/<storageIdentifier>` removed.
// ---------------------------------------------------------------------------
{
    jsTestLog("Case 2: reason=temp_files_missing");
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const coll = primary.getDB(dbName).getCollection(collName);
    assert.commandWorked(coll.insert({a: 1}));

    ResumableIndexBuildTest.runFailToResume(
        rst,
        dbName,
        collName,
        indexSpec,
        {removeTempFilesBeforeStartup: true},
        [{a: 10}, {a: 11}],
        [{a: 12}, {a: 13}],
    );

    checkLog.containsJson(primary, kLogId, {reason: "temp_files_missing"});

    rst.stopSet();
}

// ---------------------------------------------------------------------------
// Case 3: standalone_mode — repl-set node booted in standalone.
// Existing aggregate log 9871800 is left alone; per-build line 11511163 surfaces
// the buildUUID alongside the new 11511160 entry.
// ---------------------------------------------------------------------------
{
    jsTestLog("Case 3: reason=standalone_mode");
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    let primary = rst.getPrimary();
    const coll = primary.getDB(dbName).getCollection(collName);
    assert.commandWorked(coll.insert({a: 1}));

    // Stall an index build, kill the node, then bring it back in standalone mode so the
    // reconcile-result path takes the standalone branch (startup_recovery.cpp:567).
    const buildUUIDs = ResumableIndexBuildTest.runAndKill(
        rst,
        dbName,
        collName,
        [[indexSpec]],
        [{name: "hangAfterStartingIndexBuild"}],
    );
    rst.start(primary, {noCleanData: true, replSet: undefined /* drop to standalone */});

    // The aggregate log keeps its existing shape...
    checkLog.containsJson(primary, 9871800);
    // ...and the new per-build line carries the reason.
    for (const buildUUID of buildUUIDs) {
        assertReasonLogged(primary, buildUUID, "standalone_mode");
    }

    rst.stopSet();
}

// ---------------------------------------------------------------------------
// Case 4: unclean_shutdown — SIGKILL before checkpoint.
// ---------------------------------------------------------------------------
{
    jsTestLog("Case 4: reason=unclean_shutdown");
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    let primary = rst.getPrimary();
    const coll = primary.getDB(dbName).getCollection(collName);
    assert.commandWorked(coll.insert({a: 1}));

    const buildUUIDs = ResumableIndexBuildTest.runAndKill(
        rst,
        dbName,
        collName,
        [[indexSpec]],
        [{name: "hangAfterStartingIndexBuild"}],
        9 /* SIGKILL */,
    );
    rst.start(primary, {noCleanData: true});

    for (const buildUUID of buildUUIDs) {
        assertReasonLogged(primary, buildUUID, "unclean_shutdown");
    }

    rst.stopSet();
}
