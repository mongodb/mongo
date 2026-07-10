/**
 * Tests that the classic ProjectionStage reports expression memory tracking statistics when
 * featureFlagQueryMemoryTracking and featureFlagExpressionMemoryTracking are both enabled, and that
 * no statistics are reported when the flags are disabled.
 *
 * A computed projection over a document field (not a constant) is evaluated per-document by the
 * classic PROJECTION_DEFAULT stage. When memory tracking is enabled, the memory used while
 * evaluating the projection's expressions is charged to the operation memory tracker and surfaced
 * both in system.profile and in the PROJECTION_DEFAULT stage of explain("executionStats").
 *
 * @tags: [
 *   requires_profiling,
 *   queries_system_profile_collection,
 *   command_not_supported_in_serverless,
 *   requires_fcv_90,
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {getExecutionStages, getPlanStage} from "jstests/libs/query/analyze_plan.js";

// A large string field so that $concat charges a measurable amount of memory to the tracker.
const bigStr = "x".repeat(1024);
// A computed projection over a document field (not a constant) is evaluated per-document by the
// classic PROJECTION_DEFAULT stage.
const projection = {out: {$concat: ["$s", "$s"]}};

function setUpColl(conn) {
    const db = conn.getDB("test");
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insertOne({_id: 1, s: bigStr}));
    // Force the classic engine: expression memory tracking is a classic-engine mechanism.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
    );
    // Decrease the threshold for rate-limiting writes to CurOp so the op-wide peak is reported to
    // the profiler even though the projection only uses a small amount of memory.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxWriteToCurOpMemoryUsageBytes: 256}),
    );
    return {db, coll};
}

function runFindAndGetProfilerEntry(db, coll) {
    const comment = jsTestName() + "_" + ObjectId().str;
    assert.commandWorked(db.runCommand({find: coll.getName(), filter: {}, projection, comment}));

    let entries;
    assert.soon(() => {
        entries = db.system.profile.find({"command.comment": comment}).toArray();
        return entries.length >= 1;
    }, "Timed out waiting for profiler entry for comment: " + comment);
    assert.eq(1, entries.length, "unexpected profiler entries", {entries});
    return entries[0];
}

function getProjectionStage(coll) {
    const explain = coll.find({}, projection).explain("executionStats");
    const execStages = getExecutionStages(explain)[0];
    const projStage = getPlanStage(execStages, "PROJECTION_DEFAULT");
    assert.neq(null, projStage, "Expected a PROJECTION_DEFAULT stage in explain", {explain});
    return projStage;
}

// Test 1: both flags enabled — the projection's expression evaluation contributes to memory
// tracking, so peakTrackedMemBytes appears in the profiler and in the PROJECTION_DEFAULT stage.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagQueryMemoryTracking: true,
            featureFlagExpressionMemoryTracking: true,
        },
    });
    assert.neq(null, conn, "mongod was unable to start up");
    const {db, coll} = setUpColl(conn);
    db.setProfilingLevel(2, {slowms: -1});

    const entry = runFindAndGetProfilerEntry(db, coll);
    assert(
        entry.hasOwnProperty("peakTrackedMemBytes"),
        "Expected peakTrackedMemBytes when both flags enabled",
        {entry},
    );
    assert.gt(
        entry.peakTrackedMemBytes,
        0,
        "Expected positive peakTrackedMemBytes when both flags enabled",
        {entry},
    );

    const projStage = getProjectionStage(coll);
    assert(
        projStage.hasOwnProperty("peakTrackedMemBytes"),
        "Expected peakTrackedMemBytes in PROJECTION_DEFAULT stage",
        {projStage},
    );
    assert.gt(
        projStage.peakTrackedMemBytes,
        0,
        "Expected positive peakTrackedMemBytes in PROJECTION_DEFAULT stage",
        {projStage},
    );

    db.setProfilingLevel(0);
    MongoRunner.stopMongod(conn);
}

// Test 2: both flags disabled — no memory tracking at all, so no peakTrackedMemBytes anywhere.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagQueryMemoryTracking: false,
            featureFlagExpressionMemoryTracking: false,
        },
    });
    assert.neq(null, conn, "mongod was unable to start up");
    const {db, coll} = setUpColl(conn);
    db.setProfilingLevel(2, {slowms: -1});

    const entry = runFindAndGetProfilerEntry(db, coll);
    assert(
        !entry.hasOwnProperty("peakTrackedMemBytes"),
        "Unexpected peakTrackedMemBytes when both flags disabled",
        {entry},
    );

    const projStage = getProjectionStage(coll);
    assert(
        !projStage.hasOwnProperty("peakTrackedMemBytes"),
        "Unexpected peakTrackedMemBytes in PROJECTION_DEFAULT stage when flags disabled",
        {projStage},
    );

    db.setProfilingLevel(0);
    MongoRunner.stopMongod(conn);
}
