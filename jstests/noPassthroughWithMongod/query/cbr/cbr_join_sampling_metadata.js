/**
 * Verifies that ceSamplingMetadata is populated in explain output for queries that go through the
 * join optimizer.
 *
 * The join optimizer always uses sampling-based CE for each table in the join. This test confirms
 * that the resulting sampling metadata is surfaced in queryPlanner.ceSamplingMetadata, with one
 * entry per namespace.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {joinTestWrapper} from "jstests/libs/query/join_utils.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {dropSamplesColl} from "jstests/libs/query/persistent_samples_utils.js";

const ordersName = jsTestName() + "_orders";
const productsName = jsTestName() + "_products";
const orders = db[ordersName];
const products = db[productsName];

// Use a sample size smaller than the smallest collection (10 products) so that
// generateSample() takes the random path rather than the full-collection-scan path.
const joinSampleSize = 5;

describe("ceSamplingMetadata in join explain", function () {
    before(function () {
        orders.drop();
        products.drop();
        assert.commandWorked(
            orders.insertMany(
                Array.from({length: 100}, (_, i) => ({_id: i, productId: i % 10, qty: i})),
            ),
        );
        assert.commandWorked(
            products.insertMany(
                Array.from({length: 10}, (_, i) => ({_id: i, name: "product_" + i, price: i * 5})),
            ),
        );

        // Dummy indexes to provide path arrayness info required by the join optimizer.
        assert.commandWorked(orders.createIndex({dummy: 1, productId: 1}));
        assert.commandWorked(products.createIndex({dummy: 1, _id: 1}));
    });

    after(function () {
        orders.drop();
        products.drop();
    });

    it("exposes ceSamplingMetadata for all joined namespaces", function () {
        joinTestWrapper(db, () => {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalEnableJoinOptimization: true,
                    internalJoinPlanSamplingSize: joinSampleSize,
                }),
            );

            // $lookup must be followed by $unwind (without preserveNullAndEmptyArrays)
            // for the join optimizer to consider it eligible for reordering.
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

            const explain = orders.explain().aggregate(pipeline);
            const queryPlanner = getQueryPlanner(explain);

            // Confirm join optimization was used.
            assert(
                queryPlanner.winningPlan.usedJoinOptimization,
                "expected join optimization to be used",
                {
                    winningPlan: queryPlanner.winningPlan,
                },
            );

            // ceSamplingMetadata must be present since the join optimizer always uses sampling CE.
            assert(
                queryPlanner.ceSamplingMetadata,
                "expected ceSamplingMetadata in queryPlanner explain output",
                {
                    queryPlanner,
                },
            );
            const ceSamplingMetadata = queryPlanner.ceSamplingMetadata;

            // Verify both namespaces have entries.
            const ordersNs = orders.getFullName();
            const productsNs = products.getFullName();

            const validTechniques = ["random", "chunk", "fullCollScan", "seqScan", "strides"];

            const ordersMeta = ceSamplingMetadata[ordersNs];
            assert(ordersMeta, "expected ceSamplingMetadata entry for orders namespace", {
                ceSamplingMetadata,
            });
            assert.eq(ordersMeta.sampleSource, "onTheFly", "expected onTheFly sample for orders", {
                ordersMeta,
            });
            assert.eq(
                ordersMeta.sampleDocCount,
                joinSampleSize,
                "expected orders sample doc count to match requested size",
                {ordersMeta},
            );
            assert.eq(
                ordersMeta.sampleRequestedDocCount,
                joinSampleSize,
                "expected orders requested doc count to match joinSampleSize",
                {ordersMeta},
            );
            assert(
                validTechniques.includes(ordersMeta.sampleTechnique),
                "expected sampleTechnique to be a known technique",
                {ordersMeta, validTechniques},
            );

            const productsMeta = ceSamplingMetadata[productsNs];
            assert(productsMeta, "expected ceSamplingMetadata entry for products namespace", {
                ceSamplingMetadata,
            });
            assert.eq(
                productsMeta.sampleSource,
                "onTheFly",
                "expected onTheFly sample for products",
                {productsMeta},
            );
            assert.eq(
                productsMeta.sampleDocCount,
                joinSampleSize,
                "expected products sample doc count to match requested size",
                {productsMeta},
            );
            assert.eq(
                productsMeta.sampleRequestedDocCount,
                joinSampleSize,
                "expected products requested doc count to match joinSampleSize",
                {productsMeta},
            );
            assert(
                validTechniques.includes(productsMeta.sampleTechnique),
                "expected sampleTechnique to be a known technique",
                {productsMeta, validTechniques},
            );
        });
    });

    it("exposes sampleSource as 'persisted' when a persistent sample exists", function () {
        // TODO SERVER-128680 Remove this check once feature flag is removed.
        // featureFlagPersistentStats must be enabled for analyze to store samples.
        const flagRes = db.adminCommand({getParameter: 1, featureFlagPersistentStats: 1});
        if (!flagRes.ok || !flagRes.featureFlagPersistentStats.value) {
            jsTest.log.info("Skipping: featureFlagPersistentStats not enabled");
            return;
        }

        // Create persisted samples for both collections.
        assert.commandWorked(
            db.runCommand({
                analyze: ordersName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: joinSampleSize,
            }),
        );
        assert.commandWorked(
            db.runCommand({
                analyze: productsName,
                mode: "sample",
                samplingMethod: "random",
                sampleSize: joinSampleSize,
            }),
        );

        // TODO SERVER-128713: remove knob setting once persistent samples and sequential scan work
        // together. Sequential scan mode bypasses tryLoadPersistentSample, so disable it for this
        // test and restore it afterwards.
        const origSeqScan = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQuerySamplingBySequentialScan: 1}),
        ).internalQuerySamplingBySequentialScan;
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: false}),
        );

        try {
            joinTestWrapper(db, () => {
                // Set both parameters inside the wrapper so it snapshots the originals and
                // restores them on exit.
                assert.commandWorked(
                    db.adminCommand({
                        setParameter: 1,
                        internalEnableJoinOptimization: true,
                        internalJoinPlanSamplingSize: joinSampleSize,
                    }),
                );

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

                const explain = orders.explain().aggregate(pipeline);
                const queryPlanner = getQueryPlanner(explain);

                assert(
                    queryPlanner.winningPlan.usedJoinOptimization,
                    "expected join optimization to be used",
                    {
                        winningPlan: queryPlanner.winningPlan,
                    },
                );

                assert(
                    queryPlanner.ceSamplingMetadata,
                    "expected ceSamplingMetadata in queryPlanner explain output",
                    {queryPlanner},
                );
                const ceSamplingMetadata = queryPlanner.ceSamplingMetadata;

                const ordersMeta = ceSamplingMetadata[orders.getFullName()];
                assert(ordersMeta, "expected ceSamplingMetadata entry for orders namespace", {
                    ceSamplingMetadata,
                });
                assert.eq(
                    ordersMeta.sampleSource,
                    "persisted",
                    "expected persisted sample for orders",
                    {ordersMeta},
                );
                assert.eq(
                    ordersMeta.sampleDocCount,
                    joinSampleSize,
                    "expected orders sample doc count to match requested size",
                    {ordersMeta},
                );
                assert.eq(
                    ordersMeta.sampleRequestedDocCount,
                    joinSampleSize,
                    "expected orders requested doc count to match joinSampleSize",
                    {ordersMeta},
                );
                // Assert on value of "sampleTechnique" here because we explicitly set it in this testcase.
                assert.eq(
                    ordersMeta.sampleTechnique,
                    "random",
                    "expected random sampling technique for orders",
                    {ordersMeta},
                );

                const productsMeta = ceSamplingMetadata[products.getFullName()];
                assert(productsMeta, "expected ceSamplingMetadata entry for products namespace", {
                    ceSamplingMetadata,
                });
                assert.eq(
                    productsMeta.sampleSource,
                    "persisted",
                    "expected persisted sample for products",
                    {productsMeta},
                );
                assert.eq(
                    productsMeta.sampleDocCount,
                    joinSampleSize,
                    "expected products sample doc count to match requested size",
                    {productsMeta},
                );
                assert.eq(
                    productsMeta.sampleRequestedDocCount,
                    joinSampleSize,
                    "expected products requested doc count to match joinSampleSize",
                    {productsMeta},
                );
                // Assert on value of "sampleTechnique" here because we explicitly set it in this testcase.
                assert.eq(
                    productsMeta.sampleTechnique,
                    "random",
                    "expected random sampling technique for products",
                    {productsMeta},
                );
            });
        } finally {
            // TODO SERVER-128713: remove knob restore once persistent samples and sequential scan work together.
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    internalQuerySamplingBySequentialScan: origSeqScan,
                }),
            );
            dropSamplesColl(db);
        }
    });
});
