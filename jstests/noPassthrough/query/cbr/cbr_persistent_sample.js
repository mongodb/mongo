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

import {getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPersistentStats: true,
        // Explicitly disable for variants that enable this.
        internalQuerySamplingByStrides: false,
    },
});
const db = conn.getDB("test");

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    MongoRunner.stopMongod(conn);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
const kSourceSize = 1000;
const kSourceDocs = Array.from({length: kSourceSize}, (_, i) => ({
    _id: i,
    a: i,
    tag: "from_source",
}));
const kSampleSize = PersistentSamplesUtils.defaultSampleSize(db);
const kNumChunks = PersistentSamplesUtils.defaultNumChunks(db);

function resetCollections() {
    coll.drop();
    PersistentSamplesUtils.dropSamplesColl(db);
    assert.commandWorked(coll.insert(kSourceDocs));
}

function getWinningPlanMetadata(query, expectedCeSource = "Sampling") {
    const explain = coll.find(query).explain();
    const plan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, plan), "expected a COLLSCAN plan", {plan});
    assert.eq(
        plan.estimatesMetadata.ceSource,
        expectedCeSource,
        `expected ${expectedCeSource} CE source`,
        {plan},
    );
    const ceSamplingMetadata = explain.queryPlanner.ceSamplingMetadata;
    assert(ceSamplingMetadata, "expected ceSamplingMetadata in queryPlanner", {explain});
    const ns = coll.getFullName();
    const meta = ceSamplingMetadata[ns];
    assert(meta, "expected ceSamplingMetadata entry for namespace " + ns, {ceSamplingMetadata});
    return meta;
}

const prevPlanRankerConfig = getPlanRankerConfig(db);
const prevSamplingConfig = PersistentSamplesUtils.getPersistentSamplesConfig(db);

try {
    setPlanRankerConfig(db, {
        featureFlagCostBasedRanker: true,
        internalQueryPlanRanker: "costBased",
        internalQueryCBRCEMode: "samplingCE",
    });
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryDisablePlanCache: 1,
            internalQuerySamplingBySequentialScan: false,
        }),
    );

    {
        jsTest.log.info("Testing random sampling technique with a persistent sample hit");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingCEMethodForPersistentSamples: "random",
            }),
        );
        resetCollections();

        // Collect a sample
        db.runCommand({
            analyze: collName,
            mode: "sample",
            samplingMethod: "random",
            sampleSize: kSampleSize,
        });

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "persisted", "expected persisted sample on hit", {meta});
        assert.eq(meta.sampleTechnique, "random", "expected random technique", {meta});
        assert.eq(
            meta.sampleDocCount,
            kSampleSize,
            "expected docCount to match persisted sample size",
            {meta},
        );
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
        assert(
            !meta.hasOwnProperty("sampleNumChunks"),
            "random technique should not have numChunks",
            {meta},
        );
    }

    {
        jsTest.log.info("Testing random sampling technique with a persistent sample miss");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingCEMethodForPersistentSamples: "random",
            }),
        );
        resetCollections();

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "onTheFly", "expected on-the-fly sample on miss", {meta});
        assert.eq(meta.sampleTechnique, "random", "expected random technique", {meta});
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
        assert(
            !meta.hasOwnProperty("sampleNumChunks"),
            "random technique should not have numChunks",
            {meta},
        );
    }

    {
        jsTest.log.info("Testing chunk sampling technique with a persistent sample hit");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "chunk",
                internalQuerySamplingCEMethodForPersistentSamples: "chunk",
            }),
        );
        resetCollections();

        // Collect a sample
        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "chunk",
                sampleSize: kSampleSize,
                numChunks: kNumChunks,
            }),
        );

        // Chunk sampling scans full WiredTiger pages so the actual doc count may be less than
        // sampleSize; compare sampleDocCount against what was actually stored, not the request size.
        const storedDocCount = (() => {
            const uuid = PersistentSamplesUtils.getCollUUID(db, collName);
            const id = PersistentSamplesUtils.getExpectedId(
                uuid,
                "chunk",
                kSampleSize,
                1,
                kNumChunks,
            );
            const doc = PersistentSamplesUtils.getSampleDoc(
                PersistentSamplesUtils.getSamplesColl(db),
                id,
            );
            return doc[PersistentSamplesUtils.sampleDocFieldNames.docsField].length;
        })();

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "persisted", "expected persisted sample on hit", {meta});
        assert.eq(meta.sampleTechnique, "chunk", "expected chunk technique", {meta});
        assert.eq(meta.sampleNumChunks, kNumChunks, "expected numChunks to match", {meta});
        assert.eq(
            meta.sampleDocCount,
            storedDocCount,
            "expected docCount to match persisted sample size",
            {meta},
        );
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
    }

    {
        jsTest.log.info("Testing chunk sampling technique with a persistent sample miss");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "chunk",
                internalQuerySamplingCEMethodForPersistentSamples: "chunk",
            }),
        );
        resetCollections();

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "onTheFly", "expected on-the-fly sample on miss", {meta});
        assert.eq(meta.sampleTechnique, "chunk", "expected chunk technique", {meta});
        assert.eq(meta.sampleNumChunks, kNumChunks, "expected numChunks to match", {meta});
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
    }

    // TODO SERVER-127501
    // When the collection is smaller than the requested sample size, the generated sample is a full
    // collection scan. We should still successfully load a persistent sample in this case.
    const kSmallCollSize = 50;
    {
        jsTest.log.info(
            "Testing full coll scan technique with a persistent sample hit (small collection)",
        );
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingCEMethodForPersistentSamples: "random",
            }),
        );
        coll.drop();
        PersistentSamplesUtils.dropSamplesColl(db);
        assert.commandWorked(
            coll.insertMany(
                Array.from({length: kSmallCollSize}, (_, i) => ({_id: i, a: i, tag: "src"})),
            ),
        );

        assert.commandWorked(
            db.runCommand({analyze: collName, mode: "sample", samplingMethod: "random"}),
        );

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(
            meta.sampleSource,
            "persisted",
            "expected persisted sample for small collection hit",
            {meta},
        );
        assert.eq(
            meta.sampleTechnique,
            "fullCollScan",
            "expected fullCollScan technique for small collection",
            {meta},
        );
        assert.eq(
            meta.sampleDocCount,
            kSmallCollSize,
            "expected docCount to match all docs in small collection",
            {meta},
        );
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match default sample size",
            {meta},
        );
    }

    {
        jsTest.log.info(
            "Testing full coll scan technique with a persistent sample miss (small collection)",
        );
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingCEMethodForPersistentSamples: "random",
            }),
        );
        coll.drop();
        PersistentSamplesUtils.dropSamplesColl(db);
        assert.commandWorked(
            coll.insertMany(
                Array.from({length: kSmallCollSize}, (_, i) => ({_id: i, a: i, tag: "src"})),
            ),
        );

        // No analyze — on-the-fly full coll scan expected. An on-the-fly full collection scan is
        // authoritative (it scans every document), so it reports the "Metadata" CE source rather
        // than "Sampling".
        const meta = getWinningPlanMetadata({a: {$gte: 0}}, "Metadata");
        assert.eq(
            meta.sampleSource,
            "onTheFly",
            "expected on-the-fly sample for small collection miss",
            {meta},
        );
        assert.eq(
            meta.sampleTechnique,
            "fullCollScan",
            "expected fullCollScan technique for small collection",
            {meta},
        );
        assert.eq(
            meta.sampleDocCount,
            kSmallCollSize,
            "expected docCount to match all docs in small collection",
            {meta},
        );
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match default sample size",
            {meta},
        );
    }

    {
        jsTest.log.info("Testing sequential scan technique with a persistent sample hit");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingBySequentialScan: true,
                internalQuerySamplingByStrides: false,
            }),
        );
        resetCollections();

        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: kSampleSize,
            }),
        );

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "persisted", "expected persisted sample on hit", {meta});
        assert.eq(meta.sampleTechnique, "seqScan", "expected seqScan technique", {meta});
        assert.eq(
            meta.sampleDocCount,
            kSampleSize,
            "expected docCount to match persisted sample size",
            {meta},
        );
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
        assert(
            !meta.hasOwnProperty("sampleNumChunks"),
            "seqScan technique should not have numChunks",
            {meta},
        );

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: false}),
        );
    }

    {
        jsTest.log.info("Testing sequential scan technique with a persistent sample miss");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingBySequentialScan: true,
                internalQuerySamplingByStrides: false,
            }),
        );
        resetCollections();

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "onTheFly", "expected on-the-fly sample on miss", {meta});
        assert.eq(meta.sampleTechnique, "seqScan", "expected seqScan technique", {meta});
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
        assert(
            !meta.hasOwnProperty("sampleNumChunks"),
            "seqScan technique should not have numChunks",
            {meta},
        );

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: false}),
        );
    }

    {
        jsTest.log.info("Testing strides technique with a persistent sample hit");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "random",
                internalQuerySamplingBySequentialScan: false,
                internalQuerySamplingByStrides: true,
            }),
        );
        resetCollections();

        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                samplingMethod: "chunk",
                sampleSize: kSampleSize,
            }),
        );

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "persisted", "expected persisted sample on hit", {meta});
        assert.eq(meta.sampleTechnique, "strides", "expected strides technique", {meta});
        assert.eq(
            meta.sampleDocCount,
            kSampleSize,
            "expected docCount to match persisted sample size",
            {meta},
        );
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
        assert(
            !meta.hasOwnProperty("sampleNumChunks"),
            "strides technique should not have numChunks",
            {meta},
        );

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQuerySamplingByStrides: false}),
        );
    }

    {
        jsTest.log.info("Testing strides technique with a persistent sample miss");
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQuerySamplingCEMethod: "chunk",
                internalQuerySamplingBySequentialScan: false,
                internalQuerySamplingByStrides: true,
            }),
        );
        resetCollections();

        const meta = getWinningPlanMetadata({a: {$gte: 0}});
        assert.eq(meta.sampleSource, "onTheFly", "expected on-the-fly sample on miss", {meta});
        assert.eq(meta.sampleTechnique, "strides", "expected strides technique", {meta});
        assert.eq(
            meta.sampleRequestedDocCount,
            kSampleSize,
            "expected requestedDocCount to match",
            {meta},
        );
        assert(
            !meta.hasOwnProperty("sampleNumChunks"),
            "strides technique should not have numChunks",
            {meta},
        );

        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQuerySamplingByStrides: false}),
        );
    }
} finally {
    setPlanRankerConfig(db, prevPlanRankerConfig);
    PersistentSamplesUtils.setPersistentSamplesConfig(db, prevSamplingConfig);
    MongoRunner.stopMongod(conn);
}
