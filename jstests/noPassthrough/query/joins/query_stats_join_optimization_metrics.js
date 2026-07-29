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
} from "jstests/libs/query/query_stats_utils.js";

const params = getQueryStatsServerParameters();
// Disable both join opt & join plan cache.
params.setParameter.internalEnableJoinPlanCache = false;
params.setParameter.internalEnableJoinOptimization = false;
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

// The per-query expected value of each join optimization counter for the pipeline below. The
// pipeline yields a three-node join graph (orders, customers, items) with two syntactic equality
// edges (orders.a = customers.a and orders.b = items.b). Because the two joins are on distinct
// base-collection fields, no transitive implicit edge is inferred.
const kPerQueryMetrics = {
    numNamespaces: 3,
    numLookupsInSuffix: 0,
    numJoinGraphNodes: 3,
    numSyntacticEdges: 2,
    numInferredEdges: 0,
    numSyntacticExprJoinPredicates: 0,
    numSyntacticEqJoinPredicates: 2,
    numInferredEqJoinPredicates: 2,
    numInferredSingleTablePredicates: 0,
};

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
    // Plan enumeration metrics are only aggregated across the queries that missed the join plan
    // cache and thus ran enumeration. Note: numPlanEnumerations doesn't have a histogram, its just a counter.
    assert.eq(joinMetrics.numPlanEnumerations, enumerationCount, tojson(joinMetrics));
    for (const name of kPerEnumerationMetrics) {
        const counter = joinMetrics[name];
        assert(counter, `missing counter '${name}'`, {joinMetrics});
        // Sampling makes these counters a bit flaky, so just make some basic assertions.
        assert.gte(counter.sum, 0, `counter '${name}' sum`, {joinMetrics});
        assert.gte(counter.max, 0, `counter '${name}' max`, {joinMetrics});
        assert.gte(counter.min, 0, `counter '${name}' min`, {joinMetrics});
        assert.lte(counter.min, counter.max, `counter '${name}' min vs max `, {joinMetrics});
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

MongoRunner.stopMongod(conn);
