/**
 * Verifies that NDV estimates served from persisted statistics (analyze mode "ndv") are surfaced
 * in explain output as queryPlanner.fieldStatsMetadata.<ns>.ndv for queries that go through
 * the join optimizer.
 *
 * @tags: [
 *   featureFlagPersistentStats,
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {joinTestWrapper} from "jstests/libs/query/join_utils.js";

describe("persistedNdv in join explain", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPersistentStats: true,
                internalQueryEnablePersistentNDVStats: true,
            },
        });
        this.db = this.conn.getDB("test");

        this.orders = this.db[jsTestName() + "_orders"];
        this.products = this.db[jsTestName() + "_products"];
        assert.commandWorked(
            this.orders.insertMany(
                Array.from({length: 100}, (_, i) => ({_id: i, productId: i % 10, qty: i})),
            ),
        );
        // "sku" is deliberately non-unique: for unique join keys the join optimizer skips
        // estimateNDV() entirely and uses the collection cardinality.
        assert.commandWorked(
            this.products.insertMany(Array.from({length: 10}, (_, i) => ({_id: i, sku: i % 5}))),
        );
        // Dummy indexes to provide path arrayness info required by the join optimizer.
        assert.commandWorked(this.orders.createIndex({dummy: 1, productId: 1}));
        assert.commandWorked(this.products.createIndex({dummy: 1, sku: 1}));

        this.pipeline = [
            {
                $lookup: {
                    from: this.products.getName(),
                    localField: "productId",
                    foreignField: "sku",
                    as: "product",
                },
            },
            {$unwind: "$product"},
        ];
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("surfaces persistedNdv for the namespace served from persisted statistics", function () {
        assert.commandWorked(
            this.db.runCommand({analyze: this.orders.getName(), mode: "ndv", key: "productId"}),
        );
        assert.commandWorked(
            this.db.runCommand({analyze: this.products.getName(), mode: "ndv", key: "sku"}),
        );

        joinTestWrapper(this.db, () => {
            assert.commandWorked(
                this.db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
            );

            const explain = this.orders.explain().aggregate(this.pipeline);
            const queryPlanner = getQueryPlanner(explain);
            assert(
                queryPlanner.winningPlan.usedJoinOptimization,
                "expected join optimization to be used",
                {winningPlan: queryPlanner.winningPlan},
            );

            const fieldStatsMetadata = queryPlanner.fieldStatsMetadata;
            assert(fieldStatsMetadata, "expected fieldStatsMetadata", {queryPlanner});

            // The join optimizer currently requests the NDV only for the "primary key" side
            // of the edge (the smaller collection), so only products is asserted; orders'
            // statistics exist but are not consulted. Only served statistics may appear.
            assert.eq(
                Object.keys(fieldStatsMetadata),
                [this.products.getFullName()],
                "only served statistics may appear",
                {fieldStatsMetadata},
            );
            const productsNdv = fieldStatsMetadata[this.products.getFullName()].ndv;
            assert(productsNdv, "expected ndv metadata for products", {fieldStatsMetadata});
            assert.eq(productsNdv.length, 1, "expected one served entry", {productsNdv});
            assert.eq(productsNdv[0].fieldPaths, ["sku"], "unexpected fieldPaths", {productsNdv});
            assert(productsNdv[0].createdAt instanceof Date, "createdAt must be a date", {
                productsNdv,
            });
        });
    });

    it("surfaces composite persistedNdv for a two-field join", function () {
        const orders = this.db[jsTestName() + "_orders_composite"];
        const products = this.db[jsTestName() + "_products_composite"];
        assert.commandWorked(
            orders.insertMany(
                Array.from({length: 100}, (_, i) => ({_id: i, productId: i % 10, region: i % 2})),
            ),
        );
        // Both join keys are non-unique, individually and as a pair, so the join optimizer
        // requests the composite NDV instead of shortcutting through index uniqueness.
        assert.commandWorked(
            products.insertMany(
                Array.from({length: 20}, (_, i) => ({_id: i, sku: i % 5, region: i % 2})),
            ),
        );
        assert.commandWorked(orders.createIndex({dummy: 1, productId: 1, region: 1}));
        assert.commandWorked(products.createIndex({dummy: 1, sku: 1, region: 1}));

        // Deliberately unsorted key order; the persisted statistic is canonically sorted.
        assert.commandWorked(
            this.db.runCommand({analyze: products.getName(), mode: "ndv", key: ["sku", "region"]}),
        );

        // Two equality predicates on one join edge: both are $expr equalities, selecting the
        // strict tuple variant of the composite statistic.
        const pipeline = [
            {
                $lookup: {
                    from: products.getName(),
                    as: "product",
                    let: {p: "$productId", r: "$region"},
                    pipeline: [
                        {
                            $match: {
                                $expr: {
                                    $and: [{$eq: ["$sku", "$$p"]}, {$eq: ["$region", "$$r"]}],
                                },
                            },
                        },
                    ],
                },
            },
            {$unwind: "$product"},
        ];

        joinTestWrapper(this.db, () => {
            assert.commandWorked(
                this.db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
            );

            const explain = orders.explain().aggregate(pipeline);
            const queryPlanner = getQueryPlanner(explain);
            assert(queryPlanner.winningPlan.usedJoinOptimization, "expected join optimization", {
                winningPlan: queryPlanner.winningPlan,
            });

            const persistedNdv = queryPlanner.fieldStatsMetadata[products.getFullName()].ndv;
            assert(persistedNdv, "expected composite ndv metadata", {
                fieldStatsMetadata: queryPlanner.fieldStatsMetadata,
            });
            assert.eq(persistedNdv.length, 1, "expected one served entry", {persistedNdv});
            assert.eq(persistedNdv[0].fieldPaths, ["region", "sku"], "unexpected fieldPaths", {
                persistedNdv,
            });
        });
    });

    it("serves the composite statistic for mixed equality semantics", function () {
        const orders = this.db[jsTestName() + "_orders_mixed"];
        const products = this.db[jsTestName() + "_products_mixed"];
        assert.commandWorked(
            orders.insertMany(
                Array.from({length: 100}, (_, i) => ({_id: i, productId: i % 10, region: i % 2})),
            ),
        );
        assert.commandWorked(
            products.insertMany(
                Array.from({length: 20}, (_, i) => ({_id: i, sku: i % 5, region: i % 2})),
            ),
        );
        assert.commandWorked(orders.createIndex({dummy: 1, productId: 1, region: 1}));
        assert.commandWorked(products.createIndex({dummy: 1, sku: 1, region: 1}));

        assert.commandWorked(
            this.db.runCommand({analyze: products.getName(), mode: "ndv", key: ["sku", "region"]}),
        );

        // localField/foreignField contributes a regular-equality predicate on "sku" and the
        // $expr contributes a strict one on "region", so the served sketch is the variant
        // folding "sku".
        const pipeline = [
            {
                $lookup: {
                    from: products.getName(),
                    localField: "productId",
                    foreignField: "sku",
                    as: "product",
                    let: {r: "$region"},
                    pipeline: [{$match: {$expr: {$eq: ["$region", "$$r"]}}}],
                },
            },
            {$unwind: "$product"},
        ];

        joinTestWrapper(this.db, () => {
            assert.commandWorked(
                this.db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
            );

            const explain = orders.explain().aggregate(pipeline);
            const queryPlanner = getQueryPlanner(explain);
            assert(queryPlanner.winningPlan.usedJoinOptimization, "expected join optimization", {
                winningPlan: queryPlanner.winningPlan,
            });

            const persistedNdv = queryPlanner.fieldStatsMetadata[products.getFullName()].ndv;
            assert(persistedNdv, "expected composite ndv metadata", {
                fieldStatsMetadata: queryPlanner.fieldStatsMetadata,
            });
            assert.eq(persistedNdv[0].fieldPaths, ["region", "sku"], "unexpected fieldPaths", {
                persistedNdv,
            });
        });
    });

    it("omits persistedNdv when no statistics are served", function () {
        joinTestWrapper(this.db, () => {
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    internalEnableJoinOptimization: true,
                    // Disable consumption; the statistics from the previous test stay persisted.
                    internalQueryEnablePersistentNDVStats: false,
                }),
            );

            const explain = this.orders.explain().aggregate(this.pipeline);
            const queryPlanner = getQueryPlanner(explain);
            assert(queryPlanner.winningPlan.usedJoinOptimization, "expected join optimization", {
                winningPlan: queryPlanner.winningPlan,
            });

            // With consumption disabled nothing is served, so the whole section is omitted.
            assert(!queryPlanner.fieldStatsMetadata, "expected no fieldStatsMetadata", {
                queryPlanner,
            });
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    internalQueryEnablePersistentNDVStats: true,
                }),
            );
        });
    });
});
