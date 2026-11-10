/**
 * Tests that the analyze command with mode: "sample" persists a sample whose size, sampling
 * method and chunk count are taken from the corresponding query knobs.
 *
 * @tags: [
 *   # Requires featureFlagPersistentStats for persisted samples.
 *   featureFlagPersistentStats,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {runWithKnobs} from "jstests/libs/query/knob_utils.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPersistentStats: true,
        // Explicitly disable for variants that enable this.
        internalQuerySamplingByStrides: false,
    },
});
const db = conn.getDB("test");

// For future schema changes, set this value differently depending on the FCV.
const expectedSchemaVersion = 1;

const collName = jsTestName();
const coll = db[collName];
const samplesColl = PersistentSamplesUtils.getSamplesColl(db);

const defaultSampleSize = PersistentSamplesUtils.defaultSampleSize(db);
const defaultNumChunks = PersistentSamplesUtils.defaultNumChunks(db);

function cleanup() {
    coll.drop();
    PersistentSamplesUtils.dropSamplesColl(db);
}

function insertDocs(n) {
    const docs = [];
    for (let i = 0; i < n; i++) {
        docs.push({a: i, b: "str" + i});
    }
    assert.commandWorked(coll.insertMany(docs));
}

// Runs an analyze command expected to succeed and asserts the persistent samples collection
// created remains clustered on _id.
function runAnalyze(analyzeCmd) {
    assert.commandWorked(db.runCommand(analyzeCmd));
    PersistentSamplesUtils.assertSamplesCollClustered(db);
}

function runAnalyzeWith({sampleSize, samplingMethod, numChunks}) {
    const knobs = {};
    if (sampleSize !== undefined) {
        knobs.internalSamplingSizeOverride = sampleSize;
    }
    if (samplingMethod !== undefined) {
        knobs.internalQuerySamplingCEMethodForPersistentSamples = samplingMethod;
    }
    if (numChunks !== undefined) {
        knobs.internalQueryNumChunksForChunkBasedSampling = numChunks;
    }
    runWithKnobs(db, () => runAnalyze({analyze: collName, mode: "sample"}), knobs);
}

const sourceDocFields = ["a", "b"];

// =============================================================================
// internalSamplingSizeOverride knob
// =============================================================================

// Collection (20) > sample size (10), so size == sample size exactly.
cleanup();
insertDocs(20);
{
    runAnalyzeWith({sampleSize: 10, samplingMethod: "random"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: 10,
        actualSampleSize: 10,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Sample size larger than collection: clamped to numRecords (20), so size == 20 exactly.
cleanup();
insertDocs(20);
{
    runAnalyzeWith({sampleSize: 100, samplingMethod: "random"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: 100,
        actualSampleSize: 20,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Re-running analyze with the same sample size upserts (replaces) the existing sample document,
// and createdAt is updated to a strictly later timestamp.
cleanup();
insertDocs(20);
{
    runAnalyzeWith({sampleSize: 10, samplingMethod: "random"});
    const uuid = PersistentSamplesUtils.getCollUUID(db, collName);
    const expectedId = PersistentSamplesUtils.getExpectedId(uuid, expectedSchemaVersion);
    const firstCreatedAt = PersistentSamplesUtils.getSampleDoc(samplesColl, expectedId)[
        PersistentSamplesUtils.sampleDocFieldNames.createdAtField
    ];

    // Re-running analyze against an already-clustered samples collection must be a no-op create.
    runAnalyzeWith({sampleSize: 10, samplingMethod: "random"});
    const secondPages = PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: 10,
        actualSampleSize: 10,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
    assert.gt(
        secondPages[0][PersistentSamplesUtils.sampleDocFieldNames.createdAtField],
        firstCreatedAt,
        "timestamp should be later on second run of same analyze command",
    );
}

// Sampling an empty collection produces a sample document with 0 docs
cleanup();
assert.commandWorked(db.createCollection(collName));
{
    runAnalyzeWith({sampleSize: 10, samplingMethod: "random"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: 10,
        actualSampleSize: 0,
        expectedSchemaVersion: expectedSchemaVersion,
    });
}

// Non-existent collection: analyze with sample mode fails with a "collection not found" uassert
cleanup();
assert.commandFailedWithCode(
    db.runCommand({analyze: "no_such_coll", mode: "sample"}),
    13097202,
    "analyze on a non-existent collection should fail with 13097202",
);

// =============================================================================
// Default sample size
// =============================================================================

// Large collection (> default): sample size equals the calculated default, not clamped.
cleanup();
insertDocs(500);
{
    runAnalyze({analyze: collName, mode: "sample"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: defaultSampleSize,
        actualSampleSize: defaultSampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Small collection (< default): the actual sample size is clamped to numRecords, but the
// requested size is recorded in the persistent sample.
cleanup();
insertDocs(50);
{
    runAnalyze({analyze: collName, mode: "sample"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: defaultSampleSize,
        actualSampleSize: 50,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// =============================================================================
// internalSamplingSizeOverride knob
// =============================================================================

// The override knob takes precedence over the default size derived from
// samplingConfidenceInterval and samplingMarginOfError.
cleanup();
insertDocs(500);
{
    const overrideSize = 25;
    assert.neq(
        overrideSize,
        defaultSampleSize,
        "override size must differ from the derived default for this test to be meaningful",
    );
    runAnalyzeWith({sampleSize: overrideSize, samplingMethod: "random"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: overrideSize,
        actualSampleSize: overrideSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// =============================================================================
// internalQuerySamplingCEMethodForPersistentSamples and
// internalQueryNumChunksForChunkBasedSampling knobs
// =============================================================================

// Confirms the persisted method and chunk count track their knobs rather than hardcoded
// defaults, so both are set to non-default values here.
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    const numChunks = 5;
    assert.neq(
        numChunks,
        defaultNumChunks,
        "numChunks must differ from the default for this test to be meaningful",
    );
    runAnalyzeWith({sampleSize: sampleSize, samplingMethod: "chunk", numChunks: numChunks});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "chunk",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        numChunks: numChunks,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Invalid mode values/types fail to parse.
cleanup();
insertDocs(20);
{
    // Unknown enum string for mode.
    assert.commandFailedWithCode(
        db.runCommand({analyze: collName, mode: "bogus"}),
        ErrorCodes.BadValue,
        "unknown mode should fail to parse",
    );
    // Wrong BSON type for mode (expects a string).
    assert.commandFailedWithCode(
        db.runCommand({analyze: collName, mode: 1}),
        ErrorCodes.TypeMismatch,
        "non-string mode should fail to parse",
    );
}

// =============================================================================
// timeseries collection
// =============================================================================

// TODO SERVER-127022: Update this once samplingCE supports timeseries
// analyze should fail on a timeseries collection.
cleanup();
const tsCollName = jsTestName() + "_ts";
db[tsCollName].drop();
assert.commandWorked(db.createCollection(tsCollName, {timeseries: {timeField: "timestamp"}}));
assert.commandFailedWithCode(
    db.runCommand({analyze: tsCollName, mode: "sample"}),
    ErrorCodes.IllegalOperation,
    "analyze on a timeseries collection should fail with IllegalOperation",
);
db[tsCollName].drop();

// =============================================================================
// Test-only sampling modes override specified samplingMethod
// =============================================================================

// Enable internalQuerySamplingBySequentialScan
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
);

// Chunk sample when internalQuerySamplingBySequentialScan is enabled
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    runAnalyzeWith({sampleSize: sampleSize, samplingMethod: "chunk"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "seqScan",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Random sample when internalQuerySamplingBySequentialScan is enabled
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    runAnalyzeWith({sampleSize: sampleSize, samplingMethod: "random"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "seqScan",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Enable internalQuerySamplingByStrides
assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingByStrides: true}));

// Random sample when both knobs are enabled (sequential scan overrides strides sampling)
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    runAnalyzeWith({sampleSize: sampleSize, samplingMethod: "random"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "seqScan",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Disable internalQuerySamplingBySequentialScan
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: false}),
);

// Chunk sample when internalQuerySamplingByStrides is enabled
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    runAnalyzeWith({sampleSize: sampleSize, samplingMethod: "chunk"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "strides",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Random sample when internalQuerySamplingByStrides is enabled
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    runAnalyzeWith({sampleSize: sampleSize, samplingMethod: "random"});
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "strides",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Disable internalQuerySamplingByStrides
assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingByStrides: false}));

MongoRunner.stopMongod(conn);

// =============================================================================
// featureFlagPersistentStats disabled
// =============================================================================

// When the feature flag is disabled, analyze with sample mode fails with CommandNotSupported.
// Skip in environments where the flag is force-enabled for all mongods (e.g. all-feature-flags
// suites, or suites passing --additionalFeatureFlags=featureFlagPersistentStats):
// MongoRunner.runMongod() inherits such settings from TestData.setParameters, so the flag cannot
// be made to appear off.

// TODO SERVER-135439: Need to explicitly disable once the feature flag is enabled by default
const conn2 = MongoRunner.runMongod({}); // flag off by default
const db2 = conn2.getDB(jsTestName());
if (!FeatureFlagUtil.isEnabled(db2, "PersistentStats")) {
    assert.commandWorked(db2[collName].insert({a: 1}));
    assert.commandFailedWithCode(
        db2.runCommand({analyze: collName, mode: "sample"}),
        ErrorCodes.CommandNotSupported,
    );
}
MongoRunner.stopMongod(conn2);
