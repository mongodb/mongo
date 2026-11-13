/**
 * Tests that a single analyze call persists one sample per collection, and that both the CBR
 * and join optimization read paths can consume it. A consumer whose requested size exceeds the
 * persisted sample's size falls back to on-the-fly sampling; a consumer requesting less uses a
 * prefix of the persisted sample.
 *
 * @tags: [
 *   # Requires featureFlagPersistentStats for persisted samples.
 *   featureFlagPersistentStats,
 *   requires_sbe,
 * ]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {runWithKnobs} from "jstests/libs/query/knob_utils.js";
import {
    getQueryPlanner,
    getWinningPlanFromExplain,
    isCollscan,
} from "jstests/libs/query/analyze_plan.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPersistentStats: true,
        featureFlagCostBasedRanker: true,
        internalQueryPlanRanker: "costBased",
        internalQueryCBRCEMode: "samplingCE",
        internalEnableJoinOptimization: true,
        internalQueryDisablePlanCache: true,
        internalQuerySamplingCEMethod: "random",
        internalJoinOptimizationSamplingCEMethod: "random",
        internalQuerySamplingBySequentialScan: false,
        internalQuerySamplingByStrides: false,
    },
});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB(jsTestName());

const ordersName = "orders";
const productsName = "products";
const orders = db[ordersName];
const products = db[productsName];

const kNumOrders = 200;
const kNumProducts = 150;

// Sizes requested by the two read paths. They must differ so that the size reported by a query
// identifies which knob it consulted.
const kSmallerSize = 20;
const kLargerSize = 30;
assert.neq(
    kSmallerSize,
    kLargerSize,
    "the two sample sizes must differ for this test to be meaningful",
);

const kRandomSamplingMethod = "random";
const kChunkSamplingMethod = "chunk";

function setSampleSizeKnobs({cbrSampleSize, joinSampleSize}) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalSamplingSizeOverride: cbrSampleSize,
            internalJoinPlanSamplingSize: joinSampleSize,
        }),
    );
}

// Asserts that a single ceSamplingMetadata entry key reports a sample of 'expectedSize' obtained from
// 'expectedSource' ("persisted" or "onTheFly").
function assertSampleMetadata(
    meta,
    {expectedSource, expectedSize, expectedTechnique = kRandomSamplingMethod},
) {
    assert.eq(meta.sampleSource, expectedSource, `expected a '${expectedSource}' sample`, {meta});
    assert.eq(meta.sampleRequestedDocCount, expectedSize, "unexpected requested sample size", {
        meta,
    });
    if (expectedTechnique === kChunkSamplingMethod) {
        assert.between(
            PersistentSamplesUtils.defaultNumChunks(db),
            meta.sampleDocCount,
            expectedSize,
            "unexpected sampled document count",
            {meta},
        );
    } else {
        assert.eq(meta.sampleDocCount, expectedSize, "unexpected sampled document count", {meta});
    }
    assert.eq(meta.sampleTechnique, expectedTechnique, "unexpected sampling technique", {meta});
    if (expectedSource === "persisted") {
        PersistentSamplesUtils.assertPersistedSampleMetadataPresent(meta);
    } else {
        PersistentSamplesUtils.assertPersistedSampleMetadataAbsent(meta);
    }
}

// Runs a single-collection query, which is estimated by CBR, and returns its sampling metadata.
function getCbrSamplingMetadata() {
    const explain = orders.find({qty: {$gte: 0}}).explain();
    const plan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, plan), "expected a COLLSCAN plan", {plan});
    assert.eq(plan.estimatesMetadata.ceSource, "Sampling", "expected the Sampling CE source", {
        plan,
    });

    const ns = orders.getFullName();
    const ceSamplingMetadata = getQueryPlanner(explain).ceSamplingMetadata;
    assert(ceSamplingMetadata, "expected ceSamplingMetadata in queryPlanner", {explain});
    const meta = ceSamplingMetadata[ns];
    assert(meta, `expected a ceSamplingMetadata entry for ${ns}`, {ceSamplingMetadata});
    return meta;
}

// Runs a $lookup that the join optimizer takes over and returns its sampling metadata, which holds
// one entry per joined namespace.
function getJoinSamplingMetadata() {
    // $lookup must be followed by an $unwind (without preserveNullAndEmptyArrays) for the join
    // optimizer to consider it eligible for reordering.
    const pipeline = [
        {
            $lookup: {
                from: productsName,
                localField: "productId",
                foreignField: "_id",
                as: "product",
            },
        },
        {$unwind: "$product"},
    ];

    const queryPlanner = getQueryPlanner(orders.explain().aggregate(pipeline));
    assert(queryPlanner.winningPlan.usedJoinOptimization, "expected join optimization to be used", {
        winningPlan: queryPlanner.winningPlan,
    });

    const ceSamplingMetadata = queryPlanner.ceSamplingMetadata;
    assert(ceSamplingMetadata, "expected ceSamplingMetadata in queryPlanner", {queryPlanner});
    for (const coll of [orders, products]) {
        const ns = coll.getFullName();
        const meta = ceSamplingMetadata[ns];
        assert(meta, `expected a ceSamplingMetadata entry for ${ns}`, {ceSamplingMetadata});
    }
    return ceSamplingMetadata;
}

function analyzeBoth() {
    assert.commandWorked(db.runCommand({analyze: ordersName, mode: "sample"}));
    assert.commandWorked(db.runCommand({analyze: productsName, mode: "sample"}));
}

describe("the single persisted sample shared by CBR and join optimization", () => {
    before(() => {
        // A collection smaller than the requested sample size is sampled with a full collection
        // scan instead of the pinned technique, which would invalidate the assertions below.
        assert.lt(
            Math.max(kSmallerSize, kLargerSize),
            Math.min(kNumOrders, kNumProducts),
            "every collection must be larger than the largest sample size this test requests",
        );

        orders.drop();
        products.drop();
        assert.commandWorked(
            orders.insertMany(
                Array.from({length: kNumOrders}, (_, i) => ({
                    _id: i,
                    productId: i % kNumProducts,
                    qty: i,
                })),
            ),
        );
        assert.commandWorked(
            products.insertMany(
                Array.from({length: kNumProducts}, (_, i) => ({_id: i, price: i * 5})),
            ),
        );

        // Dummy indexes to provide the path arrayness info required by the join optimizer.
        assert.commandWorked(orders.createIndex({dummy: 1, productId: 1}));
        assert.commandWorked(products.createIndex({dummy: 1, _id: 1}));
    });

    after(() => {
        MongoRunner.stopMongod(conn);
    });

    beforeEach(() => {
        PersistentSamplesUtils.dropSamplesColl(db);
    });

    it("persists a single sample per collection, replacing it on re-analyze", () => {
        setSampleSizeKnobs({cbrSampleSize: kSmallerSize, joinSampleSize: kLargerSize});

        const expectedSample = {
            sampledCollName: ordersName,
            samplingMethod: kRandomSamplingMethod,
            requestedSampleSize: kLargerSize,
            actualSampleSize: kLargerSize,
        };
        assert.commandWorked(db.runCommand({analyze: ordersName, mode: "sample"}));
        const originalSamplePages = PersistentSamplesUtils.validatePersistentSample(
            db,
            expectedSample,
        );

        // A second analyze replaces the sample with the next generation rather than accumulating
        // another one.
        assert.commandWorked(db.runCommand({analyze: ordersName, mode: "sample"}));
        const updatedSamplePages = PersistentSamplesUtils.validatePersistentSample(db, {
            ...expectedSample,
            expectedGenId: PersistentSamplesUtils.kFirstPersistentSampleGenId + 1,
        });

        const createdAtField = PersistentSamplesUtils.sampleDocFieldNames.createdAtField;
        assert.gt(
            updatedSamplePages[0][createdAtField],
            originalSamplePages[0][createdAtField],
            "re-analyze should persist a fresh sample with a later createdAt",
        );
    });

    it("is loaded by both read paths when the join size fits inside it", () => {
        setSampleSizeKnobs({cbrSampleSize: kLargerSize, joinSampleSize: kSmallerSize});
        analyzeBoth();

        for (const collName of [ordersName, productsName]) {
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kLargerSize,
                actualSampleSize: kLargerSize,
            });
        }

        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kLargerSize,
        });

        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            const meta = ceSamplingMetadata[coll.getFullName()];
            assertSampleMetadata(meta, {
                expectedSource: "persisted",
                expectedSize: kSmallerSize,
            });
        }
    });

    it("is loaded by both read paths when both request the same size", () => {
        setSampleSizeKnobs({cbrSampleSize: kSmallerSize, joinSampleSize: kSmallerSize});
        analyzeBoth();

        for (const collName of [ordersName, productsName]) {
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kSmallerSize,
                actualSampleSize: kSmallerSize,
            });
        }

        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kSmallerSize,
        });

        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            const meta = ceSamplingMetadata[coll.getFullName()];
            assertSampleMetadata(meta, {
                expectedSource: "persisted",
                expectedSize: kSmallerSize,
            });
        }
    });

    it("join optimization samples on-the-fly when its requested size exceeds the persisted sample", () => {
        setSampleSizeKnobs({cbrSampleSize: kSmallerSize, joinSampleSize: kSmallerSize});
        analyzeBoth();

        for (const collName of [ordersName, productsName]) {
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kSmallerSize,
                actualSampleSize: kSmallerSize,
            });
        }

        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kSmallerSize,
        });

        // The on-the-fly technique is pinned to chunk (rather than left at its "random" default)
        // so the reported technique proves the join read path consulted its own method knob.
        const ceSamplingMetadata = runWithKnobs(db, getJoinSamplingMetadata, {
            internalJoinPlanSamplingSize: kLargerSize,
            internalJoinOptimizationSamplingCEMethod: kChunkSamplingMethod,
        });
        for (const coll of [orders, products]) {
            const meta = ceSamplingMetadata[coll.getFullName()];
            assertSampleMetadata(meta, {
                expectedSource: "onTheFly",
                expectedSize: kLargerSize,
                expectedTechnique: kChunkSamplingMethod,
            });
        }
    });
});
