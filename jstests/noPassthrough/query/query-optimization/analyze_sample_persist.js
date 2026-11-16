/**
 * Tests that the analyze command with mode: "sample" persists a sample with the right attributes
 *
 * @tags: [
 *   # Requires featureFlagPersistentStats for persisted samples.
 *   featureFlagPersistentStats,
 *   uses_parallel_shell,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getAnalyzeMetrics} from "jstests/libs/query/planning_metrics_utils.js";
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
const defaultPersistedSampleSize = PersistentSamplesUtils.expectedPersistedSampleSize(db);

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

// Returns how many persisted samples hit no write conflict and how many hit at least one.
function getWriteConflictCounts() {
    const [noConflicts, ...someConflicts] = getAnalyzeMetrics(db).histograms.writeConflicts;
    return {
        none: Number(noConflicts.count),
        some: someConflicts.reduce((sum, bucket) => sum + Number(bucket.count), 0),
    };
}

const sourceDocFields = ["a", "b"];

// =============================================================================
// internalSamplingSizeOverride knob
// =============================================================================

// Collection (20) > sample size (10), so size == sample size exactly.
cleanup();
insertDocs(20);
{
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: 10,
        }),
    );
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
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: 100,
        }),
    );
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
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: 10,
        }),
    );
    const uuid = PersistentSamplesUtils.getCollUUID(db, collName);
    const expectedId = PersistentSamplesUtils.getExpectedId(uuid, expectedSchemaVersion);
    const firstCreatedAt = PersistentSamplesUtils.getSampleDoc(samplesColl, expectedId)[
        PersistentSamplesUtils.sampleDocFieldNames.createdAtField
    ];

    // Re-running analyze against an already-clustered samples collection must replace the existing sample document.
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: 10,
        }),
    );
    const secondPages = PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: 10,
        actualSampleSize: 10,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
        expectedGenId: PersistentSamplesUtils.kFirstPersistentSampleGenId + 1,
    });
    assert.gt(
        secondPages[0][PersistentSamplesUtils.sampleDocFieldNames.createdAtField],
        firstCreatedAt,
        "timestamp should be later on second run of same analyze command",
    );
}

// Two concurrent analyze commands pick the same gen_id: the one that loses the race retries with
// the next gen_id.
cleanup();
insertDocs(20);
{
    const analyzeCmd = {analyze: collName, mode: "sample"};
    const expectedSample = {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: defaultPersistedSampleSize,
        actualSampleSize: 20,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
        expectedGenId: PersistentSamplesUtils.kFirstPersistentSampleGenId,
    };
    const writeConflictsBefore = getWriteConflictCounts();
    assert.commandWorked(PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName));
    assert.docEq(
        {none: writeConflictsBefore.none + 1, some: writeConflictsBefore.some},
        getWriteConflictCounts(),
        "an uncontended analyze should not hit a write conflict",
    );

    const fp = configureFailPoint(conn, "hangAfterFindingMaxPersistentSampleGenId");
    const startAnalyze = () =>
        startParallelShell(
            funWithArgs(
                async (dbName, cmd, expectedSample) => {
                    const {validatePersistentSample} = await import(
                        "jstests/libs/query/persistent_samples_utils.js"
                    );
                    const testDb = db.getSiblingDB(dbName);
                    validatePersistentSample(testDb, expectedSample);
                    assert.commandWorked(testDb.runCommand(cmd));
                },
                db.getName(),
                analyzeCmd,
                expectedSample,
            ),
            conn.port,
        );
    const awaitFirstAnalyze = startAnalyze();
    const awaitSecondAnalyze = startAnalyze();
    fp.wait({timesEntered: 2});
    fp.off();

    awaitFirstAnalyze();
    awaitSecondAnalyze();

    PersistentSamplesUtils.validatePersistentSample(db, {
        ...expectedSample,
        expectedGenId: PersistentSamplesUtils.kFirstPersistentSampleGenId + 2,
    });
    assert.docEq(
        {none: writeConflictsBefore.none + 2, some: writeConflictsBefore.some + 1},
        getWriteConflictCounts(),
        "only the analyze that lost the gen_id race should have hit a write conflict",
    );
}

// Two concurrent analyze commands pick the first gen_id when no generation has been persisted yet:
// the one that loses the race retries with the next gen_id.
cleanup();
insertDocs(20);
{
    const analyzeCmd = {analyze: collName, mode: "sample"};
    const writeConflictsBefore = getWriteConflictCounts();

    const fp = configureFailPoint(conn, "hangAfterFindingMaxPersistentSampleGenId");
    const startAnalyze = () =>
        startParallelShell(
            funWithArgs(
                async (dbName, cmd) => {
                    const {getCollUUID, getSampleLookupFilter, getSamplesColl} = await import(
                        "jstests/libs/query/persistent_samples_utils.js"
                    );
                    const testDb = db.getSiblingDB(dbName);
                    const filter = getSampleLookupFilter(getCollUUID(testDb, cmd.analyze));
                    assert.eq(
                        0,
                        getSamplesColl(testDb).find(filter).itcount(),
                        "expected no sample",
                        {filter},
                    );
                    assert.commandWorked(testDb.runCommand(cmd));
                },
                db.getName(),
                analyzeCmd,
            ),
            conn.port,
        );
    const awaitFirstAnalyze = startAnalyze();
    const awaitSecondAnalyze = startAnalyze();
    fp.wait({timesEntered: 2});
    fp.off();

    awaitFirstAnalyze();
    awaitSecondAnalyze();

    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: defaultPersistedSampleSize,
        actualSampleSize: 20,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
        expectedGenId: PersistentSamplesUtils.kFirstPersistentSampleGenId + 1,
    });
    assert.docEq(
        {none: writeConflictsBefore.none + 1, some: writeConflictsBefore.some + 1},
        getWriteConflictCounts(),
        "only the analyze that lost the gen_id race should have hit a write conflict",
    );
}

// Sampling an empty collection produces a sample document with 0 docs
cleanup();
assert.commandWorked(db.createCollection(collName));
{
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: 10,
        }),
    );
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
insertDocs(2000);
{
    assert.lt(
        defaultPersistedSampleSize,
        2000,
        "the collection size must exceed the default sample size",
    );
    assert.commandWorked(PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName));
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: defaultPersistedSampleSize,
        actualSampleSize: defaultPersistedSampleSize,
        expectedSchemaVersion: expectedSchemaVersion,
        expectedFields: sourceDocFields,
    });
}

// Small collection (< default): the actual sample size is clamped to numRecords, but the
// requested size is recorded in the persistent sample.
cleanup();
insertDocs(50);
{
    assert.commandWorked(PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName));
    PersistentSamplesUtils.validatePersistentSample(db, {
        sampledCollName: collName,
        samplingMethod: "random",
        requestedSampleSize: defaultPersistedSampleSize,
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
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: overrideSize,
        }),
    );
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
// internalJoinPlanSamplingSize knob
// =============================================================================

// The persisted sample is sized to the larger of the CBR and join sample sizes, regardless of
// whether join optimization is enabled.
cleanup();
insertDocs(500);
let expectedGenId = PersistentSamplesUtils.kFirstPersistentSampleGenId;
for (const enableJoinOptimization of [true, false]) {
    for (const {cbrSampleSize, joinSampleSize, expectedSampleSize} of [
        {cbrSampleSize: 10, joinSampleSize: 25, expectedSampleSize: 25},
        {cbrSampleSize: 25, joinSampleSize: 10, expectedSampleSize: 25},
    ]) {
        assert.commandWorked(
            PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
                sampleSize: cbrSampleSize,
                joinSampleSize,
                enableJoinOptimization,
                samplingMethod: "random",
            }),
        );
        PersistentSamplesUtils.validatePersistentSample(db, {
            sampledCollName: collName,
            samplingMethod: "random",
            requestedSampleSize: expectedSampleSize,
            actualSampleSize: expectedSampleSize,
            expectedSchemaVersion: expectedSchemaVersion,
            expectedFields: sourceDocFields,
            expectedGenId: expectedGenId++,
        });
    }
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
// sampleSize parameter is rejected in sample mode
// =============================================================================

cleanup();
insertDocs(20);
{
    assert.commandFailedWithCode(
        db.runCommand({analyze: collName, mode: "sample", sampleSize: 10}),
        ErrorCodes.InvalidOptions,
        'analyze with mode: "sample" and sampleSize should fail with InvalidOptions',
    );
    assert.eq(
        0,
        samplesColl.countDocuments({}),
        "no sample should be persisted when analyze is rejected",
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
// Test-only sampling modes override the default sampling method
// =============================================================================

// Enable internalQuerySamplingBySequentialScan
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
);

// Random sample when internalQuerySamplingBySequentialScan is enabled
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: sampleSize,
        }),
    );
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
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: sampleSize,
        }),
    );
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

// Random sample when internalQuerySamplingByStrides is enabled
cleanup();
insertDocs(20);
{
    const sampleSize = 10;
    assert.commandWorked(
        PersistentSamplesUtils.runAnalyzeSampleWithKnobs(db, collName, {
            sampleSize: sampleSize,
        }),
    );
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
