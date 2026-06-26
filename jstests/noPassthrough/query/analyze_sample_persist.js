/**
 * Tests that the analyze command with mode: "sample" correctly handles all sampling-related
 * parameters: sampleSize, sampleRate, samplingMethod, numChunks, and mode.
 */

import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPersistentStats: true,
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

const sourceDocFields = ["a", "b"];

// =============================================================================
// sampleSize parameter
// =============================================================================

// Custom sampleSize: collection (20) > sampleSize (10), so size == sampleSize exactly.
cleanup();
insertDocs(20);
{
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: 10,
            samplingMethod: "random",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "random",
        requestedSampleSize: 10,
        actualSampleSize: 10,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// sampleSize larger than collection: clamped to numRecords (20), so size == 20 exactly.
cleanup();
insertDocs(20);
{
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: 100,
            samplingMethod: "random",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "random",
        requestedSampleSize: 100,
        actualSampleSize: 20,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Re-run with the same sampleSize upserts (replaces) the existing sample document, and
// createdAt is updated to a strictly later timestamp.
cleanup();
insertDocs(20);
{
    const analyzeCmd = {
        analyze: collName,
        mode: "sample",
        sampleSize: 10,
        samplingMethod: "random",
    };
    assert.commandWorked(db.runCommand(analyzeCmd));
    const uuid = PersistentSamplesUtils.getCollUUID(db, collName);
    const expectedId = PersistentSamplesUtils.getExpectedId(
        uuid,
        "random",
        10,
        expectedSchemaVersion,
    );
    const firstCreatedAt = PersistentSamplesUtils.getSampleDoc(samplesColl, expectedId)[
        PersistentSamplesUtils.sampleDocFieldNames.createdAtField
    ];

    assert.commandWorked(db.runCommand(analyzeCmd));
    // verifySampleDoc() asserts count == 1, confirming the re-run upserted rather than inserted.
    const secondDoc = PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "random",
        requestedSampleSize: 10,
        actualSampleSize: 10,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
    assert.gt(
        secondDoc[PersistentSamplesUtils.sampleDocFieldNames.createdAtField],
        firstCreatedAt,
        "timestamp should be later on second run of same analyze command",
    );
}

// TODO SERVER-127501: Determine if analyze should permit this case
// Sampling an empty collection produces a sample document with 0 docs
cleanup();
assert.commandWorked(db.createCollection(collName));
{
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: 10,
            samplingMethod: "random",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "random",
        requestedSampleSize: 10,
        actualSampleSize: 0,
        expectedSchemaVersion: expectedSchemaVersion,
    });
}

// Non-existent collection: analyze with sample mode fails with the "collection not found" uassert.
cleanup();
assert.commandFailedWithCode(
    db.runCommand({analyze: "no_such_coll", mode: "sample", samplingMethod: "random"}),
    12433000,
    "analyze on a non-existent collection should fail with 12433000",
);

// =============================================================================
// Default sample size (neither sampleSize nor sampleRate provided)
// =============================================================================

// Large collection (> default): sampleSize equals the calculated default, not clamped.
cleanup();
insertDocs(500);
{
    assert.commandWorked(
        db.runCommand({analyze: collName, mode: "sample", samplingMethod: "random"}),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "random",
        requestedSampleSize: defaultSampleSize,
        actualSampleSize: defaultSampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Small collection (< default): the actual sample size is clamped to numRecords, but the sampleSize and _id fields will encode the requested size
cleanup();
insertDocs(50);
{
    assert.commandWorked(
        db.runCommand({analyze: collName, mode: "sample", samplingMethod: "random"}),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "random",
        requestedSampleSize: defaultSampleSize,
        actualSampleSize: 50,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// =============================================================================
// numChunks parameter
// =============================================================================

// samplingMethod: "chunk" with explicit numChunks succeeds.
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: sampleSize,
            samplingMethod: "chunk",
            numChunks: 5,
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "chunk",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        numChunks: 5,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// samplingMethod: "chunk" without explicit numChunks succeeds with default numChunks value.
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: sampleSize,
            samplingMethod: "chunk",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
        samplingMethod: "chunk",
        requestedSampleSize: sampleSize,
        actualSampleSize: sampleSize,
        numChunks: defaultNumChunks,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// =============================================================================
// samplingMethod parameter
// =============================================================================

// `random` and `chunk` success cases are exercised by the sampleSize
// and numChunks sections above. Here we only test error cases and IDL enum
// boundaries.

// samplingMethod must be specified in mode 'sample'
cleanup();
insertDocs(20);
assert.commandFailedWithCode(
    db.runCommand({analyze: collName, mode: "sample"}),
    12433001,
    "Missing samplingMethod should fail when using mode 'sample'",
);

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
    db.runCommand({analyze: tsCollName, mode: "sample", sampleSize: 10, samplingMethod: "random"}),
    ErrorCodes.CommandNotSupported,
    "analyze on a timeseries collection should fail with CommandNotSupported",
);
db[tsCollName].drop();

// =============================================================================
// BSON document size limit
// =============================================================================

// analyze should fail when sampled docs exceed the 16 MB BSON size limit.
cleanup();
{
    // Five documents at ~4 MB each accumulate to ~20 MB in the docs array, which exceeds
    // BSONObjMaxInternalSize (~16 MB) when building the persisted sample BSON document.
    const bigPayload = "x".repeat(4 * 1024 * 1024);
    assert.commandWorked(
        coll.insertMany(Array.from({length: 5}, (_, i) => ({_id: i, payload: bigPayload}))),
    );

    // sampleSize 5 == numRecords so all five documents are sampled deterministically.
    assert.commandFailedWithCode(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: 5,
            samplingMethod: "random",
        }),
        10334, // BSONObjectTooLarge
        "analyze should fail when the accumulated sample docs exceed the 16 MB BSON limit",
    );
}

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
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: sampleSize,
            samplingMethod: "chunk",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
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
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: sampleSize,
            samplingMethod: "random",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
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
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: sampleSize,
            samplingMethod: "random",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
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
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: sampleSize,
            samplingMethod: "chunk",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
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
    assert.commandWorked(
        db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: sampleSize,
            samplingMethod: "random",
        }),
    );
    PersistentSamplesUtils.verifySampleDoc(db, {
        sampledCollName: collName,
        mode: "sample",
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
// Skip in all-feature-flags environments: MongoRunner.runMongod() inherits
// featureFlagPersistentStats=true from TestData.setParameters, so the flag cannot be off.

if (!TestData.runAllFeatureFlagTests) {
    // TODO SERVER-112627: Need to explicitly disable once the feature flag is enabled by default
    const conn2 = MongoRunner.runMongod({}); // flag off by default
    const db2 = conn2.getDB(jsTestName());
    assert.commandWorked(db2[collName].insert({a: 1}));
    assert.commandFailedWithCode(
        db2.runCommand({analyze: collName, mode: "sample", samplingMethod: "random"}),
        ErrorCodes.CommandNotSupported,
    );
    MongoRunner.stopMongod(conn2);
}
