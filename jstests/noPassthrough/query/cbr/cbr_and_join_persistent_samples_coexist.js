/**
 * Tests that both CBR and JOO can use persistent samples with different or same sizes.
 *
 * @tags: [
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
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

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

// TODO SERVER-92589: Remove this exemption.
if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(`Skipping ${jsTestName()} as the SBE executor is not supported by CBR yet`);
    MongoRunner.stopMongod(conn);
    quit();
}

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

// Returns how many persisted sample pages exist for 'collName' at the given requested size. Zero
// means no sample of that size was ever persisted.
function numPersistedSamplePages(collName, requestedSampleSize) {
    const filter = PersistentSamplesUtils.getSampleLookupFilter(
        PersistentSamplesUtils.getCollUUID(db, collName),
        kRandomSamplingMethod,
        requestedSampleSize,
    );
    return PersistentSamplesUtils.getSamplesColl(db).find(filter).itcount();
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

    it("persists at the CBR size when analyze is not given a sampleSize", () => {
        setSampleSizeKnobs(kCbrSampleSize, kJoinSampleSize);

        analyzeColl(ordersName);
        analyzeColl(productsName);

        for (const collName of [ordersName, productsName]) {
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kCbrSampleSize,
                actualSampleSize: kCbrSampleSize,
            });
            // analyze never consults 'internalJoinPlanSamplingSize', so nothing was persisted at
            // the size join optimization asks for.
            assert.eq(
                0,
                numPersistedSamplePages(collName, kJoinSampleSize),
                "analyze should not persist a sample sized for join optimization",
                {collName},
            );
        }

        // CBR asks for the size it just persisted, so it loads the sample.
        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kCbrSampleSize,
        });

        // Join optimization asks for a different size and falls back to sampling on the fly.
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

    it("persists a join-usable sample when analyze is given an explicit sampleSize", () => {
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

            assert.eq(
                0,
                numPersistedSamplePages(collName, kCbrSampleSize),
                "analyze should not persist a sample sized for CBR",
                {collName},
            );
        }

        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            const ns = coll.getFullName();
            const meta = ceSamplingMetadata[ns];
            assertSampleMetadata(meta, {
                expectedSource: "persisted",
                expectedSize: kJoinSampleSize,
            });
        }

        // The mirror image of the previous case: only a join-sized sample exists, so CBR misses.
        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "onTheFly",
            expectedSize: kCbrSampleSize,
        });
    });

    it("loads the sample matching its own knob when both sizes are persisted", () => {
        setSampleSizeKnobs(kCbrSampleSize, kJoinSampleSize);

        for (const collName of [ordersName, productsName]) {
            // The CBR-sized sample comes from 'internalSamplingSizeOverride'; the join-sized one
            // can only be requested explicitly.
            analyzeColl(collName);
            analyzeColl(collName, kJoinSampleSize);

            // Samples of different sizes are stored side by side rather than replacing each other.
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kJoinSampleSize,
                actualSampleSize: kJoinSampleSize,
            });
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

        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            const ns = coll.getFullName();
            const meta = ceSamplingMetadata[ns];
            assertSampleMetadata(meta, {
                expectedSource: "persisted",
                expectedSize: kJoinSampleSize,
            });
        }
    });

    it("shares one persisted sample when both knobs request the same size", () => {
        setSampleSizeKnobs(kJoinSampleSize, kJoinSampleSize);

        // With the two knobs aligned, an analyze that does not specify a sampleSize produces a
        // sample that both read paths can load.
        analyzeColl(ordersName);
        analyzeColl(productsName);

        for (const collName of [ordersName, productsName]) {
            PersistentSamplesUtils.validatePersistentSample(db, {
                sampledCollName: collName,
                samplingMethod: kRandomSamplingMethod,
                requestedSampleSize: kJoinSampleSize,
                actualSampleSize: kJoinSampleSize,
            });

            assert.eq(
                0,
                numPersistedSamplePages(collName, kCbrSampleSize),
                "analyze should not persist a sample at any other size",
                {collName},
            );
        }

        assertSampleMetadata(getCbrSamplingMetadata(), {
            expectedSource: "persisted",
            expectedSize: kJoinSampleSize,
        });

        const ceSamplingMetadata = getJoinSamplingMetadata();
        for (const coll of [orders, products]) {
            const ns = coll.getFullName();
            const meta = ceSamplingMetadata[ns];
            assertSampleMetadata(meta, {
                expectedSource: "persisted",
                expectedSize: kJoinSampleSize,
            });
        }
    });
});
