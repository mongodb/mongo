/**
 * Tests that both CBR and JOO can use persistent samples with different or same sizes.
 *
 * @tags: [
 *   # Requires featureFlagPersistentStats for persisted samples.
 *   featureFlagPersistentStats,
 *   requires_sbe,
 * ]
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
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
        internalQuerySamplingCEMethodForPersistentSamples: "random",
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
const kCbrSampleSize = 20;
const kJoinSampleSize = 30;
assert.neq(
    kCbrSampleSize,
    kJoinSampleSize,
    "the two sample sizes must differ for this test to be meaningful",
);

const kRandomSamplingMethod = "random";

/**
 * Runs 'analyze' in sample mode, passing 'sampleSize' along only when one is given. Omitting it is
 * what makes 'analyze' fall back to 'internalSamplingSizeOverride'.
 */
function analyzeColl(collName, sampleSize = undefined) {
    const cmd = {analyze: collName, mode: "sample", samplingMethod: kRandomSamplingMethod};
    if (sampleSize !== undefined) {
        cmd.sampleSize = sampleSize;
    }
    assert.commandWorked(db.runCommand(cmd));
}

function setSampleSizeKnobs(cbrSampleSize, joinSampleSize) {
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
function assertSampleMetadata(meta, {expectedSource, expectedSize}) {
    assert.eq(meta.sampleSource, expectedSource, `expected a '${expectedSource}' sample`, {meta});
    assert.eq(meta.sampleRequestedDocCount, expectedSize, "unexpected requested sample size", {
        meta,
    });
    // Both collections are larger than every size this test requests, so the sample is always
    // filled to exactly the requested size.
    assert.eq(meta.sampleDocCount, expectedSize, "unexpected sampled document count", {meta});
    assert.eq(meta.sampleTechnique, kRandomSamplingMethod, "unexpected sampling technique", {meta});
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

describe("persisted sample sizes used by CBR and by join optimization", () => {
    before(() => {
        // A collection smaller than the requested sample size is sampled with a full collection
        // scan instead of the pinned technique, which would invalidate the assertions below.
        assert.lt(
            Math.max(kCbrSampleSize, kJoinSampleSize),
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

    it("does not use a persisted sample smaller than the requested size", () => {
        setSampleSizeKnobs(kCbrSampleSize, kJoinSampleSize);

        // By default analyze persists CBR's sample size.
        analyzeColl(ordersName);
        analyzeColl(productsName);

        for (const collName of [ordersName, productsName]) {
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kCbrSampleSize,
                actualSampleSize: kCbrSampleSize,
            });
        }

        // CBR asks for the size it just persisted, so it loads the sample.
        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kCbrSampleSize,
        });

        // Join optimization asks for a larger size and falls back to sampling on the fly.
        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            const ns = coll.getFullName();
            const meta = ceSamplingMetadata[ns];
            assertSampleMetadata(meta, {
                expectedSource: "onTheFly",
                expectedSize: kJoinSampleSize,
            });
        }
    });

    it("satisfies both join and CBR with one larger sample, using a prefix of the sample for the smaller request", () => {
        setSampleSizeKnobs(kCbrSampleSize, kJoinSampleSize);

        analyzeColl(ordersName, kJoinSampleSize);
        analyzeColl(productsName, kJoinSampleSize);

        for (const collName of [ordersName, productsName]) {
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kJoinSampleSize,
                actualSampleSize: kJoinSampleSize,
            });
        }

        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            assertSampleMetadata(ceSamplingMetadata[coll.getFullName()], {
                expectedSource: "persisted",
                expectedSize: kJoinSampleSize,
            });
        }

        // CBR asks for a smaller size and uses only the prefix of the persisted sample it needs.
        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kCbrSampleSize,
        });
    });

    it("replaces the persisted sample when analyze runs again with a different size", () => {
        setSampleSizeKnobs(kCbrSampleSize, kJoinSampleSize);

        for (const collName of [ordersName, productsName]) {
            analyzeColl(collName, kJoinSampleSize);
            analyzeColl(collName);

            // Only the most recently persisted sample remains
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kCbrSampleSize,
                actualSampleSize: kCbrSampleSize,
            });
        }

        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kCbrSampleSize,
        });

        // The larger join-sized sample was replaced, so join optimization falls back to sampling
        // on the fly.
        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            assertSampleMetadata(ceSamplingMetadata[coll.getFullName()], {
                expectedSource: "onTheFly",
                expectedSize: kJoinSampleSize,
            });
        }
    });
});
