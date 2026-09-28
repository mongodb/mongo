/**
 * Verifies the query latency histograms per plan selection strategy. Published under serverStatus
 * "metrics.queryLatencies.<strategy>" for strategy in {multiPlanner, costBased, singlePlan,
 * cachedPlan, joinOptimization, joinCachedPlan}. Each is a subobject holding three
 * metrics:
 *   - "histogram":  an array of {lowerBound, count} buckets (a HistogramServerStatusMetric)
 *   - "latency":    the summed latency of every observation, in microseconds
 *   - "totalCount": the number of queries observed for that strategy
 *
 * The serialization is dense and fixed-shape: every bucket is emitted, including zero-count ones,
 * so the field set never varies. FTDC depends on that, since a changed field set starts a new
 * reference document.
 *
 * Each plan ranker exercises the strategies it can produce:
 *   - multiPlanning: singlePlan, multiPlanner, cachedPlan
 *   - costBased:     singlePlan, costBased,    cachedPlan
 *   - mixed:         singlePlan, multiPlanner, costBased, cachedPlan
 *
 * Join order optimization plans outside the plan ranker and records the dedicated
 * joinOptimization / joinCachedPlan strategies.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {joinOptUsed} from "jstests/libs/query/join_utils.js";
import {checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

const kStrategies = [
    "multiPlanner",
    "costBased",
    "singlePlan",
    "cachedPlan",
    "joinOptimization",
    "joinCachedPlan",
];

// Bucket count per histogram: HistogramServerStatusMetric::pow(31, 64, 2) yields 31 boundaries,
// so 32 buckets. Update this if the bounds ever change.
const kExpectedBucketCount = 32;

/**
 * Asserts `entry` is a dense per-strategy entry: a "histogram" array of kExpectedBucketCount
 * {lowerBound, count} buckets, plus "latency" and "totalCount" counters, with "totalCount" equal
 * to the sum of the bucket counts.
 */
function assertDenseHistogramShape(strategy, entry) {
    assert.eq(
        Object.keys(entry).sort(),
        ["histogram", "latency", "totalCount"],
        () => `${strategy} must publish exactly histogram, latency and totalCount`,
        {entry},
    );

    const buckets = entry.histogram;
    assert(Array.isArray(buckets), () => `${strategy}.histogram must be an array`, {entry});
    assert.eq(
        buckets.length,
        kExpectedBucketCount,
        () =>
            `${strategy}.histogram must hold ${kExpectedBucketCount} buckets ` +
            `including zero-count buckets`,
        {buckets},
    );

    let sum = 0;
    let previousLowerBound = -1;
    for (const bucket of buckets) {
        assert(
            bucket.hasOwnProperty("lowerBound") && bucket.hasOwnProperty("count"),
            () => `${strategy} bucket is missing "lowerBound" or "count"`,
            {bucket},
        );
        // The index-to-bucket mapping must be stable: that is what FTDC relies on.
        assert.gt(
            bucket.lowerBound,
            previousLowerBound,
            () => `${strategy}.histogram lowerBounds must strictly increase`,
            {buckets},
        );
        previousLowerBound = bucket.lowerBound;
        assert.gte(bucket.count, 0, () => `${strategy} bucket has a negative count`, {bucket});
        sum += bucket.count;
    }

    assert.gte(entry.latency, 0, () => `${strategy}.latency must not be negative`, {entry});
    assert.eq(
        entry.totalCount,
        sum,
        () => `${strategy}.totalCount must equal the sum of its bucket counts`,
        {entry},
    );
}

// Asserts `explain` ran under the engine implied by the framework control.
function assertEngine(explain, frameworkControl) {
    const expected = frameworkControl === "forceClassicEngine" ? "classic" : "sbe";
    assert.eq(getEngine(explain), expected, `expected ${expected} engine`, {explain});
}

// Builds queryLatencies assertion helpers over a lazy db getter (set in before()).
function makeSuite(getDb) {
    function getQueryLatencies() {
        const metrics = getDb().serverStatus().metrics;
        const section = metrics.queryLatencies;
        assert(section, "serverStatus.metrics is missing the queryLatencies section", {metrics});
        for (const s of kStrategies) {
            assert(section.hasOwnProperty(s), () => `queryLatencies missing sub-category ${s}`, {
                section,
            });
        }
        return section;
    }

    // The number of queries observed for a strategy.
    function queriesFor(section, strategy) {
        return section[strategy].totalCount;
    }

    // The summed latency, in microseconds, observed for a strategy.
    function latencyFor(section, strategy) {
        return section[strategy].latency;
    }

    function totalQueries(section) {
        return kStrategies.reduce((sum, s) => sum + queriesFor(section, s), 0);
    }

    // Runs `fn`, then asserts that `strategy`'s totalCount strictly increased.
    function assertStrategyIncremented(strategy, fn) {
        const before = getQueryLatencies();
        fn();
        const after = getQueryLatencies();
        assert.gt(
            queriesFor(after, strategy),
            queriesFor(before, strategy),
            `expected ${strategy}.totalCount to increase`,
            {before, after},
        );
    }

    // Runs `fn`, then asserts `strategy` recorded exactly `expected` new observations.
    function assertObservations(strategy, expected, fn) {
        const before = getQueryLatencies();
        fn();
        const after = getQueryLatencies();
        assert.eq(
            queriesFor(after, strategy),
            queriesFor(before, strategy) + expected,
            `expected ${strategy}.totalCount to increase by ${expected}`,
            {before, after},
        );
    }

    return {
        db: getDb,
        getQueryLatencies,
        queriesFor,
        latencyFor,
        totalQueries,
        assertStrategyIncremented,
        assertObservations,
    };
}

// ---------------------------------------------------------------------------
// Per-strategy test cases. Each registers one it() and is composed per plan ranker strategy.
// ---------------------------------------------------------------------------

// No-index collection yields one candidate (collscan): singlePlan under every ranker.
function itSinglePlan(suite, frameworkControl) {
    it("records singlePlan for find and aggregate", function () {
        const coll = suite.db().small_coll;
        assertEngine(coll.find({a: 42}).explain(), frameworkControl);

        suite.assertStrategyIncremented("singlePlan", () => {
            assert.eq(1, coll.find({a: 42}).itcount());
        });

        // Aggregate must also feed the metric.
        suite.assertStrategyIncremented("singlePlan", () => {
            assert.eq(1, coll.aggregate([{$match: {a: 42}}]).itcount());
        });
    });
}

// Two competing indexes with a matching query: the multi-planner trial picks the winner (multiPlanner).
function itMultiPlanner(suite, frameworkControl) {
    it("records multiPlanner", function () {
        const db = suite.db();
        const coll = db.multi_planner;
        coll.drop();
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
        assert.commandWorked(coll.insert(Array.from({length: 200}, (_, i) => ({a: i % 10, b: i}))));
        assert.commandWorked(db.runCommand({planCacheClear: coll.getName()}));

        const query = {a: 5, b: {$gte: 0}};
        assertEngine(coll.find(query).explain(), frameworkControl);
        suite.assertStrategyIncremented("multiPlanner", () => {
            assert.gte(coll.find(query).itcount(), 1);
        });
    });
}

// Two indexes, no-results query over a large collection: the trial hits the work budget and CBR picks the winner.
function itCostBased(suite, frameworkControl) {
    it("records costBased", function () {
        const db = suite.db();
        const coll = db.cbr_plan;
        coll.drop();
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
        // Large collection so the no-results trial hits the work budget before EOF.
        assert.commandWorked(
            coll.insert(Array.from({length: 5001}, (_, i) => ({a: i, b: i, c: i}))),
        );
        assert.commandWorked(db.runCommand({planCacheClear: coll.getName()}));

        // No document has c == -1, so no candidate plan advances during the trial.
        const query = {a: {$gt: 0}, b: {$gt: 0}, c: -1};
        assertEngine(coll.find(query).explain(), frameworkControl);
        suite.assertStrategyIncremented("costBased", () => {
            coll.find(query).itcount();
        });
    });
}

// Once a plan-cache entry is active, an identical query reuses it (cachedPlan); the cache is ranker-independent.
function itCachedPlan(suite, frameworkControl) {
    it("records cachedPlan", function () {
        const db = suite.db();
        const coll = db.cached_plan;
        coll.drop();
        assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
        assert.commandWorked(coll.insert(Array.from({length: 200}, (_, i) => ({a: i % 10, b: i}))));
        assert.commandWorked(db.runCommand({planCacheClear: coll.getName()}));

        const query = {a: 5, b: {$gte: 0}};
        assertEngine(coll.find(query).explain(), frameworkControl);
        // Run enough times to activate the plan cache entry (inactive -> active).
        for (let i = 0; i < 5; i++) {
            coll.find(query).itcount();
        }

        suite.assertStrategyIncremented("cachedPlan", () => {
            assert.gte(coll.find(query).itcount(), 1);
        });
    });
}

// ---------------------------------------------------------------------------
// Ranker-independent cases (single-plan collscans).
// ---------------------------------------------------------------------------

// The published shape is fixed: the same field names and the same 32 bucket lowerBounds appear
// before and after observations are recorded, which is what FTDC depends on.
function itDenseHistogramShape(suite) {
    // The field names plus the bucket lowerBound sequence: everything FTDC needs to stay constant.
    function shapeOf(entry) {
        return {
            fields: Object.keys(entry).sort(),
            lowerBounds: entry.histogram.map((b) => b.lowerBound),
        };
    }

    it("publishes a fixed dense bucket set before and after recording", function () {
        const before = suite.getQueryLatencies();
        const shapeBefore = {};
        for (const s of kStrategies) {
            assertDenseHistogramShape(s, before[s]);
            shapeBefore[s] = shapeOf(before[s]);
        }

        const coll = suite.db().small_coll;
        for (let i = 0; i < 5; i++) {
            assert.eq(1, coll.find({a: i}).itcount());
        }

        const after = suite.getQueryLatencies();
        for (const s of kStrategies) {
            assertDenseHistogramShape(s, after[s]);
            assert.eq(
                shapeOf(after[s]),
                shapeBefore[s],
                () => `${s}'s published shape changed while observations were recorded`,
                {before, after},
            );
        }
        assert.gt(
            suite.queriesFor(after, "singlePlan"),
            suite.queriesFor(before, "singlePlan"),
            "expected the coll scans to be recorded under singlePlan",
            {before, after},
        );
    });
}

// The "latency" counter must advance along with the observation count.
function itLatencyCounter(suite) {
    it("accumulates latency alongside totalCount", function () {
        const before = suite.getQueryLatencies();

        const coll = suite.db().small_coll;
        assert.eq(1, coll.find({a: 42}).itcount());

        const after = suite.getQueryLatencies();
        assert.eq(
            suite.queriesFor(after, "singlePlan"),
            suite.queriesFor(before, "singlePlan") + 1,
            "expected exactly one new singlePlan observation",
            {before, after},
        );
        assert.gt(
            suite.latencyFor(after, "singlePlan"),
            suite.latencyFor(before, "singlePlan"),
            "expected singlePlan.latency to grow with the new observation",
            {before, after},
        );
    });
}

// Multi-stage pipeline runs as PlanExecutorPipeline; strategy still recorded via the $cursor summary stats.
function itAggregatePipeline(suite) {
    it("records the aggregate strategy for multi-stage pipeline executors", function () {
        const coll = suite.db().small_coll;

        suite.assertStrategyIncremented("singlePlan", () => {
            assert.gte(
                coll
                    .aggregate([{$match: {a: {$gte: 0}}}, {$group: {_id: "$b", c: {$sum: 1}}}])
                    .itcount(),
                1,
            );
        });
    });
}

// A query's total time (find + every getMore) is recorded as one observation when the cursor is
// exhausted.
function itGetMoreOneQuery(suite) {
    it("records one observation per query including getMore batches", function () {
        const db = suite.db();
        const collName = "small_coll";
        const before = suite.getQueryLatencies();

        // batchSize 20 over 200 documents: one find plus several getMores.
        const findRes = assert.commandWorked(
            db.runCommand({find: collName, filter: {}, batchSize: 20}),
        );
        let cursorId = findRes.cursor.id;
        assert.neq(cursorId, NumberLong(0), "expected an open cursor", {findRes});

        assert.eq(
            suite.totalQueries(suite.getQueryLatencies()),
            suite.totalQueries(before),
            "nothing should be recorded until the query completes",
            {before},
        );

        let getMores = 0;
        while (cursorId.compare(NumberLong(0)) !== 0) {
            const res = assert.commandWorked(
                db.runCommand({getMore: cursorId, collection: collName, batchSize: 20}),
            );
            getMores++;
            cursorId = res.cursor.id;
            if (cursorId.compare(NumberLong(0)) !== 0) {
                assert.eq(
                    suite.totalQueries(suite.getQueryLatencies()),
                    suite.totalQueries(before),
                    "an intermediate getMore must not record an observation",
                    {before, getMores},
                );
            }
        }

        assert.gt(getMores, 1, "expected the query to span multiple getMores", {getMores});

        const after = suite.getQueryLatencies();
        assert.eq(
            suite.queriesFor(after, "singlePlan"),
            suite.queriesFor(before, "singlePlan") + 1,
            `1 find + ${getMores} getMores should record exactly one observation`,
            {before, after},
        );
        assert.eq(
            suite.totalQueries(after),
            suite.totalQueries(before) + 1,
            "no other strategy should have recorded anything",
            {before, after},
        );
    });
}

// A cursor killed before exhaustion still yields exactly one observation, carrying the time of the
// operations that did complete.
function itKilledCursorRecordsOnce(suite) {
    it("records one partial observation when a cursor is killed", function () {
        const db = suite.db();
        const collName = "small_coll";

        suite.assertObservations("singlePlan", 1, () => {
            const findRes = assert.commandWorked(
                db.runCommand({find: collName, filter: {}, batchSize: 20}),
            );
            const cursorId = findRes.cursor.id;
            assert.neq(cursorId, NumberLong(0), "expected an open cursor", {findRes});

            // One getMore, then abandon the cursor: the measurement is taken when killCursors
            // destroys it.
            assert.commandWorked(
                db.runCommand({getMore: cursorId, collection: collName, batchSize: 20}),
            );
            assert.commandWorked(db.runCommand({killCursors: collName, cursors: [cursorId]}));
        });
    });
}

// A failed getMore adds nothing of its own: it is rejected before execution, so it neither records
// its own time nor binds the cursor's lifespan. The query is still observed exactly once, when
// killCursors destroys the cursor, carrying only the initial find's time.
function itFailedGetMoreRecordsOnce(suite) {
    it("records one observation, from the find, when a getMore fails", function () {
        const db = suite.db();
        const collName = "small_coll";

        const findRes = assert.commandWorked(
            db.runCommand({find: collName, filter: {}, batchSize: 20}),
        );
        const cursorId = findRes.cursor.id;
        assert.neq(cursorId, NumberLong(0), "expected an open cursor", {findRes});

        assert.commandWorked(
            db.adminCommand({
                configureFailPoint: "failCommand",
                mode: {times: 1},
                data: {
                    failCommands: ["getMore"],
                    errorCode: ErrorCodes.Interrupted,
                    failInternalCommands: false,
                },
            }),
        );
        try {
            suite.assertObservations("singlePlan", 1, () => {
                assert.commandFailedWithCode(
                    db.runCommand({getMore: cursorId, collection: collName, batchSize: 20}),
                    ErrorCodes.Interrupted,
                );
                assert.commandWorked(db.runCommand({killCursors: collName, cursors: [cursorId]}));
            });
        } finally {
            assert.commandWorked(db.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
        }
    });
}

// A command that never reaches plan selection records no strategy, so it is never observed.
function itFailedCommandRecordsNothing(suite) {
    it("records nothing for a command that fails before plan selection", function () {
        const db = suite.db();

        const before = suite.getQueryLatencies();

        assert.commandFailed(
            db.runCommand({find: "small_coll", filter: {a: {$nonExistentOperator: 1}}}),
        );
        assert.commandFailed(
            db.runCommand({aggregate: "small_coll", pipeline: [{$notAStage: {}}], cursor: {}}),
        );

        const after = suite.getQueryLatencies();
        assert.eq(
            suite.totalQueries(after),
            suite.totalQueries(before),
            "a command failing before plan selection must not be observed",
            {before, after},
        );
    });
}

// Tailable/change-stream cursors can live indefinitely, so they're excluded and never emit an observation.
function itTailableExcluded(suite) {
    it("excludes tailable cursors", function () {
        const db = suite.db();
        const coll = db.tailable_excluded;
        coll.drop();
        assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 4096}));
        assert.commandWorked(coll.insert(Array.from({length: 5}, (_, i) => ({a: i}))));

        const before = suite.getQueryLatencies();

        // Tailable cursor over a capped collection: single-plan collscan spanning getMores.
        const cursor = coll.find().tailable().batchSize(1);
        for (let i = 0; i < 5; i++) {
            assert(cursor.hasNext());
            cursor.next();
        }
        cursor.close();

        const after = suite.getQueryLatencies();
        assert.eq(
            suite.totalQueries(after),
            suite.totalQueries(before),
            "tailable cursors must be excluded from queryLatencies",
            {before, after},
        );
    });
}

// ---------------------------------------------------------------------------
// Runs cases for a given framework control and plan ranker.
// ---------------------------------------------------------------------------
function defineSuiteForRanker(frameworkControl, ranker, cases) {
    describe(`internalQueryFrameworkControl=${frameworkControl}, internalQueryPlanRanker=${ranker}`, function () {
        let db;
        const suite = makeSuite(() => db);
        let conn;

        before(function () {
            conn = MongoRunner.runMongod({
                setParameter: {
                    featureFlagCostBasedRanker: true,
                    featureFlagGetExecutorDeferredEngineChoice: true,
                    internalQueryFrameworkControl: frameworkControl,
                    internalQueryPlanRanker: ranker,
                },
            });
            assert.neq(conn, null, "mongod failed to start");
            db = conn.getDB("test");
            // No-index collection: every query over it is a single-plan collscan.
            db.small_coll.drop();
            assert.commandWorked(
                db.small_coll.insertMany(Array.from({length: 200}, (_, i) => ({a: i, b: i % 7}))),
            );
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        for (const testCase of cases) {
            testCase(suite, frameworkControl);
        }
    });
}

// Tests behaviors that are ranker-independent (single-plan collscans).
function defineLifecycleSuite() {
    describe("classic engine, recording lifecycle", function () {
        let db;
        const suite = makeSuite(() => db);
        let conn;

        before(function () {
            conn = MongoRunner.runMongod({
                setParameter: {
                    featureFlagCostBasedRanker: true,
                    internalQueryFrameworkControl: "forceClassicEngine",
                    internalQueryPlanRanker: "multiPlanning",
                },
            });
            assert.neq(conn, null, "mongod failed to start");
            db = conn.getDB("test");
            // No-index collection: every query over it is a single-plan collscan.
            db.small_coll.drop();
            assert.commandWorked(
                db.small_coll.insertMany(Array.from({length: 200}, (_, i) => ({a: i, b: i % 7}))),
            );
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        itDenseHistogramShape(suite);
        itLatencyCounter(suite);
        itAggregatePipeline(suite);
        itGetMoreOneQuery(suite);
        itKilledCursorRecordsOnce(suite);
        itFailedGetMoreRecordsOnce(suite);
        itFailedCommandRecordsNothing(suite);
        itTailableExcluded(suite);
    });
}

// Join order optimization plans outside the multi-planner / plan ranker, so its queries record the
// dedicated joinOptimization / joinCachedPlan strategies. A fresh enumeration misses the join plan
// cache (joinOptimization); an identical query recovers its plan from the cache (joinCachedPlan).
function defineJoinOptimizationSuite() {
    describe("join optimization", function () {
        let db;
        let conn;
        let base;
        let left;
        let right;
        const suite = makeSuite(() => db);
        const joinPipeline = () => [
            {$lookup: {from: left.getName(), localField: "lk", foreignField: "lk", as: "jl"}},
            {$unwind: "$jl"},
            {$lookup: {from: right.getName(), localField: "rk", foreignField: "rk", as: "jr"}},
            {$unwind: "$jr"},
        ];

        before(function () {
            conn = MongoRunner.runMongod({
                setParameter: {
                    featureFlagPathArrayness: true,
                    internalEnableJoinOptimization: true,
                    internalEnableJoinPlanCache: true,
                },
            });
            assert.neq(conn, null, "mongod failed to start");
            db = conn.getDB("test");

            const prefix = jsTestName();
            base = db[prefix + "_base"];
            left = db[prefix + "_left"];
            right = db[prefix + "_right"];
            base.drop();
            left.drop();
            right.drop();

            assert.commandWorked(
                base.insert(Array.from({length: 5}, (_, i) => ({_id: i, lk: 1, rk: 1}))),
            );
            assert.commandWorked(left.insert({_id: "left", lk: 1, a: 1}));
            assert.commandWorked(right.insert({_id: "right", rk: 1, b: 1}));
            assert.commandWorked(base.createIndex({lk: 1, rk: 1}));
            assert.commandWorked(left.createIndex({lk: 1}));
            assert.commandWorked(right.createIndex({rk: 1}));
        });

        after(function () {
            MongoRunner.stopMongod(conn);
        });

        // Runs fn and asserts `strategy` gained exactly `expected` observations and no other
        // strategy recorded anything.
        function expectOnly(strategy, expected, fn) {
            const before = suite.getQueryLatencies();
            fn();
            const after = suite.getQueryLatencies();
            assert.eq(
                suite.queriesFor(after, strategy),
                suite.queriesFor(before, strategy) + expected,
                `expected ${expected} new ${strategy} observation(s)`,
                {before, after},
            );
            assert.eq(
                suite.totalQueries(after),
                suite.totalQueries(before) + expected,
                `only ${strategy} should have recorded`,
                {before, after},
            );
        }

        it("records joinOptimization for a freshly enumerated aggregate", function () {
            const explain = base.explain().aggregate(joinPipeline());
            assert(joinOptUsed(explain), "Expected join optimization to be used", {explain});

            // This is the first execution of the shape: the join plan cache is empty, so
            // join-order enumeration runs and the query is attributed to joinOptimization.
            expectOnly("joinOptimization", 1, () => {
                assert.eq(5, base.aggregate(joinPipeline()).itcount());
            });
        });

        it("records joinCachedPlan for an identical aggregate served from the join plan cache", function () {
            // The previous identical execution populated the join plan cache; this one is a
            // cache hit and must be attributed to joinCachedPlan, not re-enumeration.
            expectOnly("joinCachedPlan", 1, () => {
                assert.eq(5, base.aggregate(joinPipeline()).itcount());
            });
        });

        it("records one joinCachedPlan observation across the aggregate and its getMores", function () {
            // batchSize 2 over 5 documents: one aggregate plus several getMores. The single
            // observation is taken only when the cursor is exhausted.
            expectOnly("joinCachedPlan", 1, () => {
                const res = assert.commandWorked(
                    db.runCommand({
                        aggregate: base.getName(),
                        pipeline: joinPipeline(),
                        cursor: {batchSize: 2},
                    }),
                );
                let cursorId = res.cursor.id;
                assert.neq(cursorId, NumberLong(0), "expected an open cursor", {res});
                while (cursorId.compare(NumberLong(0)) !== 0) {
                    const getMore = assert.commandWorked(
                        db.runCommand({getMore: cursorId, collection: base.getName()}),
                    );
                    cursorId = getMore.cursor.id;
                }
            });
        });

        it("does not record a join strategy when the same shape plans without join opt", function () {
            assert.commandWorked(
                conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}),
            );
            // The $lookup-$unwind shape is still present, but the query plans normally and must
            // not be attributed to a join strategy just because it could have been optimized.
            const before = suite.getQueryLatencies();
            assert.eq(5, base.aggregate(joinPipeline()).itcount());
            const after = suite.getQueryLatencies();
            for (const strategy of ["joinOptimization", "joinCachedPlan"]) {
                assert.eq(
                    suite.queriesFor(after, strategy),
                    suite.queriesFor(before, strategy),
                    `a non-JOO query must not create a ${strategy} observation`,
                    {before, after},
                );
            }
        });
    });
}

describe("queryLatencies serverStatus metrics by plan-selection strategy", function () {
    // Each ranker exercises the strategies it can produce on the classic engine.
    defineSuiteForRanker("forceClassicEngine", "multiPlanning", [
        itSinglePlan,
        itMultiPlanner,
        itCachedPlan,
    ]);
    defineSuiteForRanker("forceClassicEngine", "costBased", [
        itSinglePlan,
        itCostBased,
        itCachedPlan,
    ]);
    defineSuiteForRanker("forceClassicEngine", "mixed", [
        itSinglePlan,
        itMultiPlanner,
        itCostBased,
        itCachedPlan,
    ]);

    // Same per-ranker sweep under SBE; trySbeEngine lowers every eligible query, reaching the same strategies as classic.
    defineSuiteForRanker("trySbeEngine", "multiPlanning", [
        itSinglePlan,
        itMultiPlanner,
        itCachedPlan,
    ]);
    defineSuiteForRanker("trySbeEngine", "costBased", [itSinglePlan, itCostBased, itCachedPlan]);
    defineSuiteForRanker("trySbeEngine", "mixed", [
        itSinglePlan,
        itMultiPlanner,
        itCostBased,
        itCachedPlan,
    ]);

    // trySbeRestricted only lowers eligible stages ($group), plain find() falls back to classic.
    defineSuiteForRanker("trySbeRestricted", "mixed", [itAggregatePipeline]);

    // Ranker-independent behaviors.
    defineLifecycleSuite();

    // Join order optimization, which plans outside the multi-planner / plan ranker. JOO requires
    // SBE, so on classic-only builds (internalQueryFrameworkControl forced to classic per the
    // variant's TestData) the suite is not registered at all.
    if (!checkSbeCompletelyDisabled(null)) {
        defineJoinOptimizationSuite();
    }
});
