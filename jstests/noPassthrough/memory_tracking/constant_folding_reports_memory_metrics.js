/**
 * Tests that constant folding in ExpressionNary::optimize() reports memory tracking statistics
 * when featureFlagQueryMemoryTracking and featureFlagExpressionMemoryTracking are both enabled,
 * and that no statistics are reported when both flags are disabled.
 *
 * When all operands of $concatArrays are constants, the expression is folded during optimization.
 * The folded evaluation runs with the OperationMemoryUsageTracker attached, so peak memory from
 * building the concatenated array is reflected in peakTrackedMemBytes.
 *
 * @tags: [
 *   requires_profiling,
 *   queries_system_profile_collection,
 *   command_not_supported_in_serverless,
 *   requires_fcv_82,
 *   assumes_against_mongod_not_mongos,
 * ]
 */

// Two constant arrays large enough to produce measurable memory during folding.
const arr1 = Array.from({length: 100}, (_, i) => i);
const arr2 = Array.from({length: 100}, (_, i) => i + 100);
const pipeline = [{$project: {a: {$concatArrays: [arr1, arr2]}}}];

function runPipelineAndGetProfilerEntry(db, coll) {
    const comment = jsTestName() + "_" + ObjectId().str;
    assert.commandWorked(db.runCommand({aggregate: coll.getName(), pipeline, comment, cursor: {}}));

    let entries;
    assert.soon(() => {
        entries = db.system.profile.find({"command.comment": comment}).toArray();
        return entries.length >= 1;
    }, "Timed out waiting for profiler entry for comment: " + comment);
    assert.eq(1, entries.length, tojson(entries));
    return entries[0];
}

// Test 1: both flags enabled — constant folding should contribute to peakTrackedMemBytes.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagQueryMemoryTracking: true,
            featureFlagExpressionMemoryTracking: true,
        },
    });
    assert.neq(null, conn, "mongod was unable to start up");
    const db = conn.getDB("test");
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insertOne({_id: 1}));
    db.setProfilingLevel(2, {slowms: -1});

    const entry = runPipelineAndGetProfilerEntry(db, coll);
    assert(
        entry.hasOwnProperty("peakTrackedMemBytes"),
        "Expected peakTrackedMemBytes when both flags enabled: " + tojson(entry),
    );
    assert.gt(
        entry.peakTrackedMemBytes,
        0,
        "Expected positive peakTrackedMemBytes when both flags enabled: " + tojson(entry),
    );

    db.setProfilingLevel(0);
    MongoRunner.stopMongod(conn);
}

// Test 2: both flags disabled — no memory tracking at all, so no peakTrackedMemBytes.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            featureFlagQueryMemoryTracking: false,
            featureFlagExpressionMemoryTracking: false,
        },
    });
    assert.neq(null, conn, "mongod was unable to start up");
    const db = conn.getDB("test");
    const coll = db[jsTestName()];
    coll.drop();
    assert.commandWorked(coll.insertOne({_id: 1}));
    db.setProfilingLevel(2, {slowms: -1});

    const entry = runPipelineAndGetProfilerEntry(db, coll);
    assert(
        !entry.hasOwnProperty("peakTrackedMemBytes"),
        "Unexpected peakTrackedMemBytes when both flags disabled: " + tojson(entry),
    );

    db.setProfilingLevel(0);
    MongoRunner.stopMongod(conn);
}
