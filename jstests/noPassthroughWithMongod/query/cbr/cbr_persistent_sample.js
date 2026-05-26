/**
 * End-to-end coverage for the persistent-sample path of sampling CE.
 *
 * When sampling CE is enabled, the estimator first tries to load a previously persisted
 * sample from `<db>.system.stats.samples` falling back to on-the-fly sampling if no
 * persistent sample is found.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
import {getCBRConfig, setCBRConfig} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

// TODO SERVER-112627: Remove once the feature flag is enabled by default
if (!FeatureFlagUtil.isEnabled(db, "PersistentStats")) {
    jsTest.log.info(`Skipping ${jsTestName()} because featureFlagPersistentStats is not enabled`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
const samplesColl = db.getCollection("system.stats.samples");
const kSchemaVersion = 1;
const kSourceSize = 1000;

// Mirror of C++ getZScore() in sampling_estimator_impl.cpp.
function getZScore(ci) {
    if (ci === "90") return 1.645;
    if (ci === "95") return 1.96;
    if (ci === "99") return 2.576;
    throw new Error(`Unknown confidence interval: ${ci}`);
}

// Mirror of C++ SamplingEstimatorImpl::calculateSampleSize().
function calculateSampleSize(ci, marginOfError) {
    const ciWidth = (2 * marginOfError) / 100.0;
    return Math.round(getZScore(ci) ** 2 / ciWidth ** 2);
}

// Read the knobs that drive sample size and chunk count so the test stays correct if defaults
// change, rather than hardcoding values that can silently drift from what the server computes.
const {
    samplingConfidenceInterval: kCI,
    samplingMarginOfError: kMoE,
    internalQueryNumChunksForChunkBasedSampling: kNumChunks,
} = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        samplingConfidenceInterval: 1,
        samplingMarginOfError: 1,
        internalQueryNumChunksForChunkBasedSampling: 1,
    }),
);
const kSampleSize = calculateSampleSize(kCI, kMoE);

const kSourceDocs = Array.from({length: kSourceSize}, (_, i) => ({_id: i, a: i, tag: "from_source"}));

function resetCollections() {
    coll.drop();
    dropSamplesCollection();
    assert.commandWorked(coll.insert(kSourceDocs));
}

// Return the source collection's UUID as a canonical hex string. This matches the format
// `buildPersistentSampleId` produces via `UUID::toString()` on the server; the raw
// `UUID().toString()` in the shell prints `UUID("…")` wrapped form which would never match.
function getCollectionUuidString() {
    const infos = db.getCollectionInfos({name: collName});
    assert.eq(infos.length, 1, infos);
    return extractUUIDFromObject(infos[0].info.uuid);
}

// Mirror of the C++ `buildPersistentSampleId` in persistent_sample_loader.h.
// Format: <UUID>_<method>_<sampleSize>_v<schemaVersion>
//     or: <UUID>_chunk<numChunks>_<sampleSize>_v<schemaVersion>
function buildPersistentSampleId(collectionUuid, method, sampleSize, numChunks) {
    if (method === "chunk") {
        return `${collectionUuid}_chunk${numChunks}_${sampleSize}_v${kSchemaVersion}`;
    }
    return `${collectionUuid}_${method}_${sampleSize}_v${kSchemaVersion}`;
}

function buildPersistentSampleDoc({
    collectionUuid,
    method,
    sampleSize,
    docs,
    numChunks = null,
    schemaVersion = kSchemaVersion,
    overrides = {},
}) {
    const base = {
        _id: buildPersistentSampleId(collectionUuid, method, sampleSize, numChunks),
        collectionUuid,
        // schemaVersion and numChunks are typed `int` in persistent_sample.idl. JS numbers
        // serialize to BSON double by default, so wrap them in NumberInt() — otherwise IDL
        // parsing rejects the doc with TypeMismatch.
        schemaVersion: NumberInt(schemaVersion),
        createdAt: new Date(),
        sampleSize: NumberLong(sampleSize),
        samplingMethod: method,
        docs,
    };
    if (numChunks !== null) {
        base.numChunks = NumberInt(numChunks);
    }
    return Object.assign(base, overrides);
}

// TODO SERVER-124330. Insert samples via analyze command.
function insertPersistedSample(doc) {
    // Samples cannot be inserted via normal operations since system collections are special
    // and need to be whitelisted.
    assert.commandWorked(
        db.adminCommand({
            applyOps: [
                {op: "c", ns: db.getName() + ".$cmd", o: {create: samplesColl.getName()}},
                {op: "i", ns: samplesColl.getFullName(), o: doc},
            ],
        }),
    );
}

function dropSamplesCollection() {
    // TODO SERVER-124350. Drop the samples collection without this hack.
    // This is needed because system collections are special and need to be whitelisted for dropping individually.
    // Not whitelisting it here since we're expecting this to happen at SERVER-124350.
    db.adminCommand({
        applyOps: [{op: "c", ns: db.getName() + ".$cmd", o: {drop: samplesColl.getName()}}],
    });
}

function getWinningPlanMetadata(query) {
    const explain = coll.find(query).explain();
    const plan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, plan), "expected a COLLSCAN plan", {plan});
    assert.eq(plan.estimatesMetadata.ceSource, "Sampling", "expected Sampling CE source", {plan});
    const ceSamplingMetadata = explain.queryPlanner.ceSamplingMetadata;
    assert(ceSamplingMetadata, "expected ceSamplingMetadata in queryPlanner", {explain});
    const ns = coll.getFullName();
    const meta = ceSamplingMetadata[ns];
    assert(meta, "expected ceSamplingMetadata entry for namespace " + ns, {ceSamplingMetadata});
    return meta;
}

const prevCBRConfig = getCBRConfig(db);
const prevSamplingConfig = assert.commandWorked(
    db.adminCommand({
        getParameter: 1,
        internalQueryDisablePlanCache: 1,
        internalQuerySamplingCEMethod: 1,
        internalQuerySamplingBySequentialScan: 1,
    }),
);

try {
    setCBRConfig(db, {
        featureFlagCostBasedRanker: true,
        internalQueryCBRCEMode: "samplingCE",
    });
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryDisablePlanCache: 1,
            internalQuerySamplingBySequentialScan: false,
        }),
    );

    const kSampleDocs = Array.from({length: kSampleSize}, (_, i) => ({_id: i, a: i}));

    {
        jsTest.log.info("Testing random sampling technique with a persistent sample hit");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "random"}));
        resetCollections();

        insertPersistedSample(
            buildPersistentSampleDoc({
                collectionUuid: getCollectionUuidString(),
                method: "random",
                sampleSize: kSampleSize,
                docs: kSampleDocs,
            }),
        );
        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "persisted", "expected persisted sample on hit", {meta});
        assert.eq(meta.sampleTechnique, "random", "expected random technique", {meta});
        assert.eq(meta.sampleDocCount, kSampleSize, "expected docCount to match persisted sample size", {meta});
        assert.eq(meta.sampleRequestedDocCount, kSampleSize, "expected requestedDocCount to match", {meta});
        assert(!meta.hasOwnProperty("sampleNumChunks"), "random technique should not have numChunks", {meta});
    }

    {
        jsTest.log.info("Testing random sampling technique with a persistent sample miss");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "random"}));
        resetCollections();

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "onTheFly", "expected on-the-fly sample on miss", {meta});
        assert.eq(meta.sampleTechnique, "random", "expected random technique", {meta});
        assert.eq(meta.sampleRequestedDocCount, kSampleSize, "expected requestedDocCount to match", {meta});
        assert(!meta.hasOwnProperty("sampleNumChunks"), "random technique should not have numChunks", {meta});
    }

    {
        jsTest.log.info("Testing chunk sampling technique with a persistent sample hit");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "chunk"}));
        resetCollections();

        insertPersistedSample(
            buildPersistentSampleDoc({
                collectionUuid: getCollectionUuidString(),
                method: "chunk",
                sampleSize: kSampleSize,
                docs: kSampleDocs,
                numChunks: kNumChunks,
            }),
        );
        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "persisted", "expected persisted sample on hit", {meta});
        assert.eq(meta.sampleTechnique, "chunk", "expected chunk technique", {meta});
        assert.eq(meta.sampleNumChunks, kNumChunks, "expected numChunks to match", {meta});
        assert.eq(meta.sampleDocCount, kSampleSize, "expected docCount to match persisted sample size", {meta});
        assert.eq(meta.sampleRequestedDocCount, kSampleSize, "expected requestedDocCount to match", {meta});
    }

    {
        jsTest.log.info("Testing chunk sampling technique with a persistent sample miss");
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "chunk"}));
        resetCollections();

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "onTheFly", "expected on-the-fly sample on miss", {meta});
        assert.eq(meta.sampleTechnique, "chunk", "expected chunk technique", {meta});
        assert.eq(meta.sampleNumChunks, kNumChunks, "expected numChunks to match", {meta});
        assert.eq(meta.sampleRequestedDocCount, kSampleSize, "expected requestedDocCount to match", {meta});
    }
} finally {
    setCBRConfig(db, prevCBRConfig);
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryDisablePlanCache: prevSamplingConfig.internalQueryDisablePlanCache,
            internalQuerySamplingCEMethod: prevSamplingConfig.internalQuerySamplingCEMethod,
            internalQuerySamplingBySequentialScan: prevSamplingConfig.internalQuerySamplingBySequentialScan,
        }),
    );
}
