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
