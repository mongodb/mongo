/**
 * Validates the join optimization metrics reported in serverStatus.
 *
 * @tags: [
 *   featureFlagPersistentStats,
 *   requires_fcv_91,
 *   requires_sbe,
 *   requires_capped,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {dropSamplesColl} from "jstests/libs/query/persistent_samples_utils.js";
import {getPersistentNDVMetrics} from "jstests/libs/query/planning_metrics_utils.js";

describe("join optimization serverStatus metrics", function () {
    const kCounters = [
        "joinOptimizationConsidered",
        "joinOptimizedTrue",
        "numEnumerations",
        "numFinalPlanHashJoins",
        "numFinalPlanIndexedNestedLoopJoins",
        "numFinalPlanNestedLoopJoins",
        "numSamplingCalls",
        "numPersistentSamplesUsed",
        "numPersistentNDVStatsUsed",
        "numSuffixSourcesPushedToSbe",
        "numResidualClassicSources",
    ];

    const kHistograms = [
        "numJoinGraphNodes",
        "numPlansEnumerated",
        "numMemoizedNodes",
        "numJoinNodesRejectedByCost",
        "joinModelingTimeMicros",
        "planEnumerationTimeMicros",
        "sbeLoweringTimeMicros",
        "cbrPlanningTimeMicros",
        "samplingTimeMicros",
        "ceTimeMicros",
    ];

    function getMetrics(conn) {
        const section = conn.getDB("admin").serverStatus().metrics.query.joinOptimization;
        assert(section, "missing joinOptimization serverStatus section");
        return section;
    }

    // Returns the total number of data points recorded in the named histogram.
    function histogramCount(metrics, name) {
        const buckets = metrics.histograms[name];
        assert(buckets, `missing histogram '${name}'`, {metrics});
        return buckets.reduce((total, bucket) => total + bucket.count, 0);
    }

    // Returns the increase in the named counter between two metrics snapshots.
    function counterDelta(before, after, name) {
        return after[name] - before[name];
    }

    // Returns the increase in the named histogram's total data point count between two snapshots.
    function histogramDelta(before, after, name) {
        return histogramCount(after, name) - histogramCount(before, name);
    }

    // Returns the reasons whose counts increased between two snapshots, mapped to their increase.
    function fallbackReasonDelta(before, after) {
        const delta = {};
        for (const [reason, count] of Object.entries(after.fallbackReasons)) {
            const increase = count - (before.fallbackReasons[reason] ?? 0);
            if (increase !== 0) {
                delta[reason] = increase;
            }
        }
        return delta;
    }

    function seed(coll, numDocs) {
        coll.drop();
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({_id: i, a: i, b: i % 10});
        }
        assert.commandWorked(coll.insertMany(docs));
        assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    }

    // Note that the test cases below share a single mongod and therefore observe cumulative
    // metrics; they all assert on the change across a snapshot taken before the query, so they are
    // independent of the order in which they run.
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: false,
                // Required for join optimization to load a persisted sample rather than always
                // sampling on the fly.
                featureFlagPersistentStats: true,
                // Lets the persisted NDV cases below serve statistics; with none persisted this
                // only makes the other cases count lookup misses, which they do not assert on.
                internalQueryEnablePersistentNDVStats: true,
            },
        });
        assert.neq(null, this.conn, "mongod was unable to start up");

        this.db = this.conn.getDB(jsTestName());
        this.orders = this.db.orders;
        this.customers = this.db.customers;
        this.items = this.db.items;

        seed(this.orders, 100);
        seed(this.customers, 100);
        seed(this.items, 100);

        // A pipeline with two $lookups yields a three-node join graph with two edges.
        this.pipeline = [
            {
                $lookup: {
                    from: this.customers.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "customer",
                },
            },
            {$unwind: "$customer"},
            {
                $lookup: {
                    from: this.items.getName(),
                    localField: "b",
                    foreignField: "b",
                    as: "item",
                },
            },
            {$unwind: "$item"},
        ];

        this.metrics = () => getMetrics(this.conn);

        // Asserts that the given query bails out of join optimization before any per-query metrics
        // are collected, so that only the fallback reason is recorded.
        this.assertEarlyFallback = (reason, runQuery) => {
            const before = this.metrics();
            runQuery();
            const after = this.metrics();
            const delta = fallbackReasonDelta(before, after);
            // Note: we may have additional fallback reasons from background queries- don't count them.
            assert.eq(delta[reason], 1, `fallback reason '${reason}' should have been recorded`, {
                before,
                after,
            });
            assert.eq(
                counterDelta(before, after, "joinOptimizationConsidered"),
                0,
                "an ineligible query should not be counted as considered",
                {before, after},
            );
        };
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    // Helpers for the persisted NDV statistics cases below. Each case joins against its own
    // products collection, so statistics persisted by one case cannot leak into another; the
    // join key ("sku", matched by orders.b) is deliberately non-unique, since the optimizer
    // skips estimateNDV() entirely for unique join keys.
    before(function () {
        this.createProducts = (suffix) => {
            const products = this.db["products_" + suffix];
            assert.commandWorked(
                products.insertMany(Array.from({length: 10}, (_, i) => ({_id: i, sku: i % 5}))),
            );
            // A dummy index provides the path arrayness info the join optimizer requires.
            assert.commandWorked(products.createIndex({dummy: 1, sku: 1}));
            return products;
        };

        this.persistentNdvMetrics = () => getPersistentNDVMetrics(this.db);

        this.runNdvJoinQuery = (products) => {
            const pipeline = [
                {
                    $lookup: {
                        from: products.getName(),
                        localField: "b",
                        foreignField: "sku",
                        as: "product",
                    },
                },
                {$unwind: "$product"},
            ];
            assert.commandWorked(
                this.db.runCommand({aggregate: this.orders.getName(), pipeline, cursor: {}}),
            );
        };
    });

    it("registers all metrics at startup", function () {
        // All metrics are registered at startup, so every counter and histogram is reported even
        // if no query has recorded a data point yet.
        const metrics = this.metrics();
        for (const name of kCounters) {
            // Counters are reported as NumberLong, so compare numerically rather than by type.
            assert.gte(metrics[name], 0, `counter '${name}' is not reported`, {metrics});
        }
        for (const name of kHistograms) {
            // Throws if the histogram is missing.
            histogramCount(metrics, name);
        }
        assert(metrics.fallbackReasons, "missing 'fallbackReasons' subsection", {metrics});
    });

    it("records one data point in every histogram for an optimizable pipeline", function () {
        // A join-optimizable pipeline is both considered and optimized, and records one data point
        // in every histogram: this run misses the (disabled) join plan cache and therefore also
        // enumerates.
        const before = this.metrics();
        assert.eq(
            this.orders.aggregate(this.pipeline, {cursor: {batchSize: 100000}}).itcount(),
            1000,
        );
        const after = this.metrics();

        const attr = {before, after};
        assert.eq(counterDelta(before, after, "joinOptimizationConsidered"), 1, "considered", attr);
        assert.eq(counterDelta(before, after, "joinOptimizedTrue"), 1, "optimized", attr);
        assert.eq(counterDelta(before, after, "numEnumerations"), 1, "enumerations", attr);
        // One sample is generated per distinct namespace in the join graph.
        assert.eq(counterDelta(before, after, "numSamplingCalls"), 3, "sampling calls", attr);
        // The winning plan joins three nodes, so it contains exactly two joins.
        assert.eq(
            counterDelta(before, after, "numFinalPlanHashJoins") +
                counterDelta(before, after, "numFinalPlanIndexedNestedLoopJoins") +
                counterDelta(before, after, "numFinalPlanNestedLoopJoins"),
            2,
            "joins in the winning plan",
            attr,
        );
        for (const name of kHistograms) {
            assert.eq(histogramDelta(before, after, name), 1, `histogram '${name}'`, attr);
        }
        assert.docEq({}, fallbackReasonDelta(before, after), "query was fully optimized", attr);
    });

    it("skips enumeration on a join plan cache hit", function () {
        // A cache hit still builds the join model and lowers to SBE, but skips enumeration
        // entirely.
        assert.commandWorked(
            this.db.adminCommand({setParameter: 1, internalEnableJoinPlanCache: true}),
        );
        try {
            const before = this.metrics();
            // The first run with the cache enabled populates it; the second hits it.
            assert.eq(
                this.orders.aggregate(this.pipeline, {cursor: {batchSize: 100000}}).itcount(),
                1000,
            );
            assert.eq(
                this.orders.aggregate(this.pipeline, {cursor: {batchSize: 100000}}).itcount(),
                1000,
            );
            const after = this.metrics();

            const attr = {before, after};
            assert.eq(
                counterDelta(before, after, "joinOptimizationConsidered"),
                2,
                "considered",
                attr,
            );
            assert.eq(counterDelta(before, after, "joinOptimizedTrue"), 2, "optimized", attr);
            // Only the first of the two runs enumerates; the second hits the cache.
            assert.eq(counterDelta(before, after, "numEnumerations"), 1, "enumerations", attr);
            assert.eq(
                histogramDelta(before, after, "numJoinGraphNodes"),
                2,
                "per-query histogram",
                attr,
            );
            assert.eq(
                histogramDelta(before, after, "planEnumerationTimeMicros"),
                1,
                "per-enumeration histogram",
                attr,
            );
        } finally {
            assert.commandWorked(
                this.db.adminCommand({setParameter: 1, internalEnableJoinPlanCache: false}),
            );
        }
    });

    it("counts persisted samples reused instead of sampling on the fly", function () {
        // Join optimization samples once per distinct namespace in the join graph. With a persisted
        // sample available for each, every sampling call loads it instead of scanning.
        const sampleSize = assert.commandWorked(
            this.db.adminCommand({getParameter: 1, internalJoinPlanSamplingSize: 1}),
        ).internalJoinPlanSamplingSize;
        const colls = [this.orders, this.customers, this.items];
        try {
            for (const coll of colls) {
                // The persisted sample is keyed on the sampling method and sample size, so both
                // must match what join optimization asks for.
                assert.commandWorked(
                    this.db.runCommand({
                        analyze: coll.getName(),
                        mode: "sample",
                        samplingMethod: "random",
                        sampleSize: sampleSize,
                    }),
                );
            }

            const before = this.metrics();
            assert.eq(
                this.orders.aggregate(this.pipeline, {cursor: {batchSize: 100000}}).itcount(),
                1000,
            );
            const after = this.metrics();

            const attr = {before, after};
            assert.eq(counterDelta(before, after, "numSamplingCalls"), 3, "sampling calls", attr);
            assert.eq(
                counterDelta(before, after, "numPersistentSamplesUsed"),
                3,
                "persisted samples reused",
                attr,
            );
        } finally {
            dropSamplesColl(this.db);
        }
    });

    it("counts suffix sources left in the classic engine", function () {
        // $facet cannot be lowered into SBE, so it ends the join-optimizable prefix and remains as
        // a residual classic DocumentSource.
        const before = this.metrics();
        assert.eq(
            this.orders
                .aggregate([...this.pipeline, {$facet: {counts: [{$count: "n"}]}}], {
                    cursor: {batchSize: 100000},
                })
                .itcount(),
            1,
        );
        const after = this.metrics();

        const attr = {before, after};
        assert.eq(counterDelta(before, after, "joinOptimizedTrue"), 1, "optimized", attr);
        assert.eq(
            counterDelta(before, after, "numResidualClassicSources"),
            1,
            "residual classic sources",
            attr,
        );
        assert.eq(
            counterDelta(before, after, "numSuffixSourcesPushedToSbe"),
            0,
            "suffix sources pushed to SBE",
            attr,
        );
    });

    it("records 'userHintPresent'", function () {
        this.assertEarlyFallback("userHintPresent", () =>
            assert.eq(this.orders.aggregate(this.pipeline, {hint: {a: 1, b: 1}}).itcount(), 1000),
        );
    });

    it("records 'collectionMissing'", function () {
        this.assertEarlyFallback("collectionMissing", () =>
            assert.eq(
                this.orders
                    .aggregate([
                        {
                            $lookup: {
                                from: "nonexistent",
                                localField: "a",
                                foreignField: "a",
                                as: "x",
                            },
                        },
                        {$unwind: "$x"},
                        ...this.pipeline,
                    ])
                    .itcount(),
                0,
            ),
        );
    });

    it("records 'collectionCapped'", function () {
        const capped = this.db.capped;
        capped.drop();
        assert.commandWorked(
            this.db.createCollection(capped.getName(), {capped: true, size: 4096}),
        );
        assert.commandWorked(capped.insertMany([{a: 1, b: 1}]));
        try {
            this.assertEarlyFallback("collectionCapped", () =>
                this.orders
                    .aggregate([
                        {
                            $lookup: {
                                from: capped.getName(),
                                localField: "a",
                                foreignField: "a",
                                as: "x",
                            },
                        },
                        {$unwind: "$x"},
                        ...this.pipeline,
                    ])
                    .itcount(),
            );
        } finally {
            capped.drop();
        }
    });

    it("records 'collectionIsView'", function () {
        assert.commandWorked(
            this.db.createView("customersView", this.customers.getName(), [
                {$match: {a: {$gte: 0}}},
            ]),
        );
        try {
            this.assertEarlyFallback("collectionIsView", () =>
                this.orders
                    .aggregate([
                        {
                            $lookup: {
                                from: "customersView",
                                localField: "a",
                                foreignField: "a",
                                as: "x",
                            },
                        },
                        {$unwind: "$x"},
                        ...this.pipeline,
                    ])
                    .itcount(),
            );
        } finally {
            this.db.customersView.drop();
        }
    });

    // The reasons below are detected by the pipeline-shape check, which runs before any collection
    // is inspected.

    it("records 'pipelineCollation'", function () {
        this.assertEarlyFallback("pipelineCollation", () =>
            this.orders
                .aggregate(this.pipeline, {collation: {locale: "en_US", strength: 2}})
                .itcount(),
        );
    });

    it("records 'lookupNotUnwound'", function () {
        // A $lookup that is not unwound produces an array rather than a join.
        this.assertEarlyFallback("lookupNotUnwound", () =>
            this.orders
                .aggregate([
                    {
                        $lookup: {
                            from: this.customers.getName(),
                            localField: "a",
                            foreignField: "a",
                            as: "c",
                        },
                    },
                ])
                .itcount(),
        );
    });

    it("records 'outerJoinUnwind'", function () {
        // Preserving null/empty arrays makes the $unwind an outer join.
        this.assertEarlyFallback("outerJoinUnwind", () =>
            this.orders
                .aggregate([
                    {
                        $lookup: {
                            from: this.customers.getName(),
                            localField: "a",
                            foreignField: "a",
                            as: "c",
                        },
                    },
                    {$unwind: {path: "$c", preserveNullAndEmptyArrays: true}},
                ])
                .itcount(),
        );
    });

    it("records 'unwindIncludeArrayIndex'", function () {
        this.assertEarlyFallback("unwindIncludeArrayIndex", () =>
            this.orders
                .aggregate([
                    {
                        $lookup: {
                            from: this.customers.getName(),
                            localField: "a",
                            foreignField: "a",
                            as: "c",
                        },
                    },
                    {$unwind: {path: "$c", includeArrayIndex: "i"}},
                ])
                .itcount(),
        );
    });

    it("records 'ineligiblePrefixStage'", function () {
        // A stage before the first $lookup that the prefix cannot represent.
        this.assertEarlyFallback("ineligiblePrefixStage", () =>
            this.orders.aggregate([{$sort: {a: 1}}, ...this.pipeline]).itcount(),
        );
    });

    it("records 'ineligibleSubPipelineStage'", function () {
        // A $lookup sub-pipeline stage that join optimization does not support.
        this.assertEarlyFallback("ineligibleSubPipelineStage", () =>
            this.orders
                .aggregate([
                    {
                        $lookup: {
                            from: this.customers.getName(),
                            let: {oa: "$a"},
                            pipeline: [{$sort: {a: 1}}, {$match: {$expr: {$eq: ["$a", "$$oa"]}}}],
                            as: "c",
                        },
                    },
                    {$unwind: "$c"},
                ])
                .itcount(),
        );
    });

    it("records 'unsupportedStage' alongside the usual metrics", function () {
        // A stage the join model cannot absorb ends the join-optimizable prefix: the query is still
        // optimized over that prefix, so the reason is recorded alongside the usual metrics.
        const before = this.metrics();
        assert.eq(
            this.orders
                .aggregate([...this.pipeline, {$group: {_id: "$b", n: {$sum: 1}}}], {
                    cursor: {batchSize: 100000},
                })
                .itcount(),
            10,
        );
        const after = this.metrics();

        const attr = {before, after};
        assert.docEq(
            {unsupportedStage: 1},
            fallbackReasonDelta(before, after),
            "only fallback reason 'unsupportedStage' should have been recorded",
            attr,
        );
        assert.eq(counterDelta(before, after, "joinOptimizedTrue"), 1, "optimized", attr);
        // The $group is lowered into SBE rather than left in the classic suffix.
        assert.eq(
            counterDelta(before, after, "numSuffixSourcesPushedToSbe"),
            1,
            "suffix sources pushed to SBE",
            attr,
        );
    });

    it("counts a persistentNdv miss when no statistics are persisted", function () {
        const products = this.createProducts("miss");

        const before = this.persistentNdvMetrics();
        this.runNdvJoinQuery(products);
        const after = this.persistentNdvMetrics();

        assert.eq(after.misses, before.misses + 1, "expected one miss", {before, after});
        assert.eq(after.hits, before.hits, "expected no hit", {before, after});
    });

    it("counts a persistentNdv hit and numPersistentNDVStatsUsed when statistics serve", function () {
        const products = this.createProducts("hit");
        assert.commandWorked(
            this.db.runCommand({analyze: products.getName(), mode: "ndv", key: "sku"}),
        );

        const before = this.persistentNdvMetrics();
        const joinOptBefore = this.metrics();
        this.runNdvJoinQuery(products);
        const after = this.persistentNdvMetrics();
        const joinOptAfter = this.metrics();

        assert.eq(after.hits, before.hits + 1, "expected one hit", {before, after});
        assert.eq(after.misses, before.misses, "expected no miss", {before, after});
        // The same use aggregates into this section's counter as well.
        assert.eq(
            counterDelta(joinOptBefore, joinOptAfter, "numPersistentNDVStatsUsed"),
            1,
            "expected one persisted NDV statistic counted",
            {joinOptBefore, joinOptAfter},
        );
    });

    it("counts neither hit nor miss when persisted NDV consumption is disabled", function () {
        const products = this.createProducts("disabled");
        assert.commandWorked(
            this.db.runCommand({analyze: products.getName(), mode: "ndv", key: "sku"}),
        );

        assert.commandWorked(
            this.db.adminCommand({setParameter: 1, internalQueryEnablePersistentNDVStats: false}),
        );
        try {
            const before = this.persistentNdvMetrics();
            this.runNdvJoinQuery(products);
            const after = this.persistentNdvMetrics();

            assert.eq(after.hits, before.hits, "expected no hit", {before, after});
            assert.eq(after.misses, before.misses, "expected no miss", {before, after});
        } finally {
            assert.commandWorked(
                this.db.adminCommand({
                    setParameter: 1,
                    internalQueryEnablePersistentNDVStats: true,
                }),
            );
        }
    });
});
