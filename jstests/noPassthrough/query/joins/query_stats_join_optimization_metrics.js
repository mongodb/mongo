/**
 * Validates the join optimization metrics reported in the query stats supplemental metrics section.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {
    getQueryStats,
    getQueryStatsServerParameters,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

const params = getQueryStatsServerParameters();
// Disable both join opt & join plan cache.
params.setParameter.internalEnableJoinPlanCache = false;
params.setParameter.internalEnableJoinOptimization = false;
// Needed to materialize and consume a persistent sample below.
// TODO SERVER-112627: Remove once featureFlagPersistentStats is enabled by default.
params.setParameter.featureFlagPersistentStats = true;
// A persisted sample is keyed by the requested sample size, so join optimization only reuses one if
// it asks for exactly the size that was persisted.
const kJoinSampleSize = 50;
params.setParameter.internalJoinPlanSamplingSize = kJoinSampleSize;
const conn = MongoRunner.runMongod(params);
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());
const orders = db.orders;
const customers = db.customers;
const items = db.items;

function seed(coll, numDocs) {
    coll.drop();
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({_id: i, a: i, b: i % 10});
    }
    assert.commandWorked(coll.insertMany(docs));
    // An index gives the optimizer multikeyness metadata and enables indexed nested loop joins.
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
}

seed(orders, 100);
seed(customers, 100);
seed(items, 100);

// Materialize a persistent sample for exactly one of the three collections.
assert.commandWorked(
    db.runCommand({
        analyze: customers.getName(),
        mode: "sample",
        samplingMethod: "random",
        sampleSize: kJoinSampleSize,
    }),
);

// The per-query expected value of each join optimization counter for the pipeline below. The
// pipeline yields a three-node join graph (orders, customers, items) with two syntactic equality
// edges (orders.a = customers.a and orders.b = items.b). Because the two joins are on distinct
// base-collection fields, no transitive implicit edge is inferred.
const kPerQueryMetrics = {
    numNamespaces: 3,
    numLookupsInSuffix: 0,
    numSuffixSourcesPushedToSbe: 0,
    numResidualClassicSources: 0,
    numJoinGraphNodes: 3,
    numSyntacticEdges: 2,
    numInferredEdges: 0,
    numSyntacticExprJoinPredicates: 0,
    numSyntacticEqJoinPredicates: 2,
    numInferredEqJoinPredicates: 2,
    numInferredSingleTablePredicates: 0,
};

// Timers recorded on every join-optimization attempt, regardless of whether we hit the join plan
// cache: we always build the join model and always lower the chosen plan to SBE. Each one covers
// enough real work (canonical query construction, SBE stage building) to round up to at least one
// microsecond, so every data point - and therefore 'min' - must be positive.
const kPerQueryTimers = ["joinModelingTimeMicros", "sbeLoweringTimeMicros"];

// Plan enumeration metrics are only populated on a join plan cache miss, so they only ever reflect
// the enumeration data points (i.e. the number of cache misses), not the total 'updateCount'.
const kPerEnumerationMetrics = [
    "numPlansEnumerated",
    "numHashJoins",
    "numIndexedNestedLoopJoins",
    "numNestedLoopJoins",
    "numFinalPlanHashJoins",
    "numFinalPlanIndexedNestedLoopJoins",
    "numFinalPlanNestedLoopJoins",
    "numMemoizedNodes",
    "numJoinNodesRejectedByCost",
    "winningPlanCost",
];

// Plan enumeration counters whose per-enumeration value is deterministic for a given pipeline.
const kPerEnumerationExpectedMetrics = {
    // One sample is generated per distinct namespace in the join graph.
    numSamplingCalls: 3,
    // Only 'customers' has a persisted sample to reuse.
    numPersistentSamplesUsed: 1,
    numUniqueIndexesUsedForNDV: 0,
};

// Timers recorded only on the enumeration path, i.e. only on a join plan cache miss. Like the
// per-query timers, each covers enough work (sampling the collections, running CBR, walking the
// enumeration lattice) that every recorded data point must be positive.
const kPerEnumerationTimers = [
    "samplingTimeMicros",
    "cbrPlanningTimeMicros",
    "planEnumerationTimeMicros",
    "ceTimeMicros",
];

// Asserts a timing histogram is populated: every data point was positive, so all values >=0, and
// 'sum' covers 'updateCount' data points each at least as large as 'min' and at most 'max'.
function assertTimerPopulated(joinMetrics, name, updateCount) {
    const counter = joinMetrics[name];
    assert(counter, `missing timer '${name}'`, {joinMetrics});
    assert.gte(counter.min, 0, `timer '${name}' min`, {joinMetrics});
    assert.lte(counter.min, counter.max, `timer '${name}' min vs max`, {joinMetrics});
    assert.gte(counter.sum, counter.min * updateCount, `timer '${name}' sum`, {joinMetrics});
    assert.lte(counter.sum, counter.max * updateCount, `timer '${name}' sum`, {joinMetrics});
}

// Asserts every join optimization counter, given the number of times the (identical) query has been
// aggregated into the query stats entry. Each per-query value is identical across runs, so the
// aggregated 'sum' is 'value * updateCount' while 'min' and 'max' equal the per-query value.
function assertJoinMetrics(joinMetrics, updateCount, enumerationCount) {
    assert.eq(joinMetrics.updateCount, updateCount, tojson(joinMetrics));
    // Every run of this pipeline is join-optimizable.
    assert.docEq(
        {"true": NumberLong(updateCount), "false": NumberLong(0)},
        joinMetrics.joinOptimizable,
        tojson(joinMetrics),
    );
    for (const [name, perQuery] of Object.entries(kPerQueryMetrics)) {
        const counter = joinMetrics[name];
        assert(counter, `missing counter '${name}'`, {joinMetrics});
        assert.eq(counter.sum, perQuery * updateCount, `counter '${name}' sum`, {joinMetrics});
        assert.eq(counter.max, perQuery, `counter '${name}' max`, {joinMetrics});
        assert.eq(counter.min, perQuery, `counter '${name}' min`, {joinMetrics});
    }
    for (const name of kPerQueryTimers) {
        assertTimerPopulated(joinMetrics, name, updateCount);
    }
    // Plan enumeration metrics are only aggregated across the queries that missed the join plan
    // cache and thus ran enumeration. Note: numPlanEnumerations doesn't have a histogram, its just a counter.
    assert.eq(joinMetrics.numPlanEnumerations, enumerationCount, tojson(joinMetrics));
    for (const name of kPerEnumerationTimers) {
        assertTimerPopulated(joinMetrics, name, enumerationCount);
    }
    for (const name of kPerEnumerationMetrics) {
        const counter = joinMetrics[name];
        assert(counter, `missing counter '${name}'`, {joinMetrics});
        // Sampling makes these counters a bit flaky, so just make some basic assertions.
        assert.gte(counter.sum, 0, `counter '${name}' sum`, {joinMetrics});
        assert.gte(counter.max, 0, `counter '${name}' max`, {joinMetrics});
        assert.gte(counter.min, 0, `counter '${name}' min`, {joinMetrics});
        assert.lte(counter.min, counter.max, `counter '${name}' min vs max `, {joinMetrics});
    }
    for (const [name, perEnumeration] of Object.entries(kPerEnumerationExpectedMetrics)) {
        const counter = joinMetrics[name];
        assert(counter, `missing counter '${name}'`, {joinMetrics});
        assert.eq(counter.sum, perEnumeration * enumerationCount, `counter '${name}' sum`, {
            joinMetrics,
        });
        if (enumerationCount > 0) {
            assert.eq(counter.max, perEnumeration, `counter '${name}' max`, {joinMetrics});
            assert.eq(counter.min, perEnumeration, `counter '${name}' min`, {joinMetrics});
        }
    }
}

// A pipeline with two $lookups yields a three-node join graph with two edges.
const pipeline = [
    {
        $lookup: {from: customers.getName(), localField: "a", foreignField: "a", as: "customer"},
    },
    {$unwind: "$customer"},
    {
        $lookup: {from: items.getName(), localField: "b", foreignField: "b", as: "item"},
    },
    {$unwind: "$item"},
];
assert.eq(orders.aggregate(pipeline, {cursor: {batchSize: 100000}}).itcount(), 1000);

// With join optimization disabled, no JoinOptimization supplemental metrics should be present.
{
    const stats = getQueryStats(conn, {collName: orders.getName()});
    assert.eq(1, stats.length, tojson(stats));

    const supplementalMetrics = stats[0].metrics.supplementalMetrics;
    assert(
        !supplementalMetrics || !supplementalMetrics.JoinOptimization,
        tojson(supplementalMetrics),
    );
}

// Enable the join optimizer so that a multi-$lookup pipeline is reordered and produces join
// optimization metrics.
assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));

// Run the query so that it is registered in the query stats store.
assert.eq(orders.aggregate(pipeline, {cursor: {batchSize: 100000}}).itcount(), 1000);

// With join optimization enabled, we expect to see some metrics.
{
    const stats = getQueryStats(conn, {collName: orders.getName()});
    assert.eq(1, stats.length, tojson(stats));

    const joinMetrics = stats[0].metrics.supplementalMetrics.JoinOptimization;
    assert(joinMetrics);
    assertJoinMetrics(joinMetrics, 1, 1);
}

// Enable the join plan cache.
assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinPlanCache: true}));

// Run the query again!
assert.eq(orders.aggregate(pipeline, {cursor: {batchSize: 100000}}).itcount(), 1000);

// We expect to see updated metrics.
{
    const stats = getQueryStats(conn, {collName: orders.getName()});
    assert.eq(1, stats.length, tojson(stats));

    const joinMetrics = stats[0].metrics.supplementalMetrics.JoinOptimization;
    assert(joinMetrics);
    // The second run still enumerates.
    assertJoinMetrics(joinMetrics, 2, 2);
}

// Now repeat, but the plan should be cached.
assert.eq(orders.aggregate(pipeline, {cursor: {batchSize: 100000}}).itcount(), 1000);

{
    const stats = getQueryStats(conn, {collName: orders.getName()});
    assert.eq(1, stats.length, tojson(stats));

    const joinMetrics = stats[0].metrics.supplementalMetrics.JoinOptimization;
    assert(joinMetrics);
    // The third run does not enumerate.
    assertJoinMetrics(joinMetrics, 3, 2);
}

{
    // The plan cache key ignores the pipeline suffix, so this variant could hit the cache and skip
    // suffix pushdown (TODO SERVER-130469). Disable the cache to keep the metrics deterministic.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinPlanCache: false}));

    // Validate that we correctly count pushed down vs classic stages.
    const suffixPipeline = [
        ...pipeline,
        {$group: {_id: "$b", n: {$sum: 1}}},
        {$_internalInhibitOptimization: {}},
        {$match: {n: {$gte: 0}}},
    ];

    // Update expected metrics to reflect this.
    kPerQueryMetrics.numSuffixSourcesPushedToSbe = 1;
    kPerQueryMetrics.numResidualClassicSources = 2;

    assert.eq(orders.aggregate(suffixPipeline, {cursor: {batchSize: 100000}}).itcount(), 10);

    const stats = getQueryStats(conn, {collName: orders.getName()});
    const matching = stats.filter((s) => tojson(s.key.queryShape).includes("$group"));
    assert.eq(1, matching.length, "expected exactly one query shape with a $group", {stats});

    const joinMetrics = matching[0].metrics.supplementalMetrics.JoinOptimization;
    assert(joinMetrics);
    assertJoinMetrics(joinMetrics, 1, 1);
}

{
    // Validate that we count the join edges whose NDV came from index uniqueness metadata. Run the
    // same pipeline as above, but recreate one of the join key indexes as unique so that the optimizer can
    // derive NDV from that metadata.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalEnableJoinOptimizationUseIndexUniqueness: true}),
    );
    for (const coll of [orders, customers, items]) {
        assert.commandWorked(coll.dropIndex({a: 1, b: 1}));
        assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
        assert.commandWorked(coll.createIndex({b: 1}));
    }

    // This is the same query shape as the runs above, so clear the query stats store to get
    // histograms that cover only the run below.
    resetQueryStatsStore(conn, "10MB");

    // Update expected metrics to reflect this: only the 'orders.a = customers.a' edge has a unique
    // index covering its join key.
    kPerQueryMetrics.numSuffixSourcesPushedToSbe = 0;
    kPerQueryMetrics.numResidualClassicSources = 0;
    kPerEnumerationExpectedMetrics.numUniqueIndexesUsedForNDV = 1;

    assert.eq(orders.aggregate(pipeline, {cursor: {batchSize: 100000}}).itcount(), 1000);

    const stats = getQueryStats(conn, {collName: orders.getName()});
    assert.eq(1, stats.length, tojson(stats));

    const joinMetrics = stats[0].metrics.supplementalMetrics.JoinOptimization;
    assert(joinMetrics);
    // Recreating the indexes invalidated the join plan cache, so this run enumerates again.
    assertJoinMetrics(joinMetrics, 1, 1);
}

MongoRunner.stopMongod(conn);
