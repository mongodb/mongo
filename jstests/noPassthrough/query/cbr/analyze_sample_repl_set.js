/**
 * Tests that analyze with mode: "sample" replicates persisted samples to secondaries and
 * that secondaries use the persisted sample for cardinality estimation.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_90,
 * ]
 */

import {after, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kDbName = jsTestName();
const kCollName = "coll";

const rst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {
            featureFlagCostBasedRanker: true,
            featureFlagPersistentStats: true,
            internalQueryCBRCEMode: "samplingCE",
            internalQuerySamplingCEMethod: "random",
            internalQuerySamplingBySequentialScan: false,
        },
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB = primary.getDB(kDbName);
const secondaryDB = secondary.getDB(kDbName);

// TODO SERVER-112627: Remove once featureFlagPersistentStats is enabled by default.
if (!FeatureFlagUtil.isEnabled(primaryDB, "PersistentStats")) {
    jsTest.log.info(`Skipping ${jsTestName()}: featureFlagPersistentStats is not enabled`);
    rst.stopSet();
    quit();
}

const kSampleSize = PersistentSamplesUtils.defaultSampleSize(primaryDB);
// The collection must be larger than the default sample size so the persisted sample is not
// clamped to the collection size
const kNumDocs = 2 * kSampleSize;
const kDocs = Array.from({length: kNumDocs}, (_, i) => ({_id: i, a: i, b: i % 10}));

assert.commandWorked(primaryDB[kCollName].insertMany(kDocs));
rst.awaitReplication();

/**
 * Returns the ceSamplingMetadata entry for kCollName from an explain run on the secondary,
 * or null if the field is absent.
 */
function getSecondaryMeta(query) {
    const explain = secondaryDB[kCollName].find(query).explain();
    const allMeta = explain?.queryPlanner?.ceSamplingMetadata;
    if (!allMeta) {
        return null;
    }
    return allMeta[secondaryDB[kCollName].getFullName()] ?? null;
}

function resetStats() {
    PersistentSamplesUtils.dropSamplesColl(primaryDB);
    rst.awaitReplication();
}

function assertSampleDocReplicatedExactly(expectedId) {
    const primarySamplesColl = primaryDB[PersistentSamplesUtils.samplesCollName];
    const secondarySamplesColl = secondaryDB[PersistentSamplesUtils.samplesCollName];

    const primaryDoc = primarySamplesColl.findOne({_id: expectedId});
    assert(primaryDoc, "Expected sample doc on primary", {expectedId});

    assert.soon(
        () => secondarySamplesColl.findOne({_id: expectedId}) !== null,
        "sample document did not replicate to secondary",
    );
    const secondaryDoc = secondarySamplesColl.findOne({_id: expectedId});

    assert.docEq(
        primaryDoc,
        secondaryDoc,
        "sample document on secondary does not exactly match the primary",
    );
}

describe("analyze sample on replica sets", function () {
    beforeEach(function () {
        resetStats();
    });

    after(function () {
        rst.stopSet();
    });

    describe("replication", function () {
        it("random sample document replicates from primary to secondary", function () {
            assert.commandWorked(
                primaryDB.runCommand({
                    analyze: kCollName,
                    mode: "sample",
                    sampleSize: kSampleSize,
                    samplingMethod: "random",
                }),
            );
            rst.awaitReplication();

            const uuid = PersistentSamplesUtils.getCollUUID(primaryDB, kCollName);
            const expectedId = PersistentSamplesUtils.getExpectedId(uuid, "random", kSampleSize);

            assertSampleDocReplicatedExactly(expectedId);
        });

        it("chunk sample document replicates from primary to secondary", function () {
            const kNumChunks = PersistentSamplesUtils.defaultNumChunks(primaryDB);
            assert.commandWorked(
                primaryDB.runCommand({
                    analyze: kCollName,
                    mode: "sample",
                    sampleSize: kSampleSize,
                    samplingMethod: "chunk",
                    numChunks: kNumChunks,
                }),
            );
            rst.awaitReplication();

            const uuid = PersistentSamplesUtils.getCollUUID(primaryDB, kCollName);
            const expectedId = PersistentSamplesUtils.getExpectedId(
                uuid,
                "chunk",
                kSampleSize,
                1 /* schemaVersion */,
                kNumChunks,
            );

            assertSampleDocReplicatedExactly(expectedId);
        });

        it("re-running analyze updates the sample document on the secondary", function () {
            // First analyze run.
            assert.commandWorked(
                primaryDB.runCommand({
                    analyze: kCollName,
                    mode: "sample",
                    sampleSize: kSampleSize,
                    samplingMethod: "random",
                }),
            );
            rst.awaitReplication();

            const uuid = PersistentSamplesUtils.getCollUUID(primaryDB, kCollName);
            const expectedId = PersistentSamplesUtils.getExpectedId(uuid, "random", kSampleSize);
            const secondarySamplesColl = secondaryDB[PersistentSamplesUtils.samplesCollName];
            const firstDoc = secondarySamplesColl.findOne({_id: expectedId});
            assert(firstDoc, "Expected sample doc on secondary after first analyze", {expectedId});

            // Second analyze run with the same parameters should upsert (update) the document.
            assert.commandWorked(
                primaryDB.runCommand({
                    analyze: kCollName,
                    mode: "sample",
                    sampleSize: kSampleSize,
                    samplingMethod: "random",
                }),
            );
            rst.awaitReplication();

            // Still exactly one document with the same _id.
            const allDocs = secondarySamplesColl.find({_id: expectedId}).toArray();
            assert.eq(allDocs.length, 1, "Expected exactly one sample doc after re-run", {allDocs});

            // createdAt must be strictly later on the second run.
            const secondDoc = allDocs[0];
            assert.gt(
                secondDoc[PersistentSamplesUtils.sampleDocFieldNames.createdAtField],
                firstDoc[PersistentSamplesUtils.sampleDocFieldNames.createdAtField],
                "createdAt should be later after re-running analyze",
            );
        });
    });

    // TODO SERVER-92589: Remove once CBR supports SBE.
    if (checkSbeFullyEnabled(primaryDB)) {
        jsTest.log.info(`Skipping remaining tests: ${jsTestName()}: CBR does not support SBE`);
        rst.stopSet();
        quit();
    }

    describe("CE on secondary uses persisted sample", function () {
        it("uses on-the-fly sampling when no persisted sample exists", function () {
            const uuid = PersistentSamplesUtils.getCollUUID(primaryDB, kCollName);
            const expectedId = PersistentSamplesUtils.getExpectedId(uuid, "random", kSampleSize);
            const secondarySamplesColl = secondaryDB[PersistentSamplesUtils.samplesCollName];
            const firstDoc = secondarySamplesColl.findOne({_id: expectedId});
            assert(firstDoc == null, "Expected sample doc on secondary not to exist", {expectedId});

            const meta = getSecondaryMeta({a: {$gte: 0}});
            assert(meta, "Expected ceSamplingMetadata in explain on secondary");
            assert.eq(
                meta.sampleSource,
                "onTheFly",
                "Expected onTheFly sample source before analyze",
                {meta},
            );
        });

        it("uses persisted sample after analyze replicates", function () {
            assert.commandWorked(
                primaryDB.runCommand({
                    analyze: kCollName,
                    mode: "sample",
                    sampleSize: kSampleSize,
                    samplingMethod: "random",
                }),
            );
            rst.awaitReplication();

            const meta = getSecondaryMeta({a: {$gte: 0}});
            assert(meta, "Expected ceSamplingMetadata in explain on secondary after analyze");
            assert.eq(
                meta.sampleSource,
                "persisted",
                "Expected persisted sample source after analyze replicates to secondary",
                {meta},
            );
            assert.eq(meta.sampleTechnique, "random", "Expected random technique", {meta});
            assert.eq(meta.sampleDocCount, kSampleSize, "Expected docCount to match kSampleSize", {
                meta,
            });
        });
    });
});
