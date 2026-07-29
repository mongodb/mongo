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

const conn = MongoRunner.runMongod(getQueryStatsServerParameters());
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());

// Start with join opt disabled.
assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));

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
    assert.eq(joinMetrics.updateCount, 1, tojson(joinMetrics));
}

// Run the query again!
assert.eq(orders.aggregate(pipeline, {cursor: {batchSize: 100000}}).itcount(), 1000);

// With join optimization enabled, we expect to see updated metrics.
{
    const stats = getQueryStats(conn, {collName: orders.getName()});
    assert.eq(1, stats.length, tojson(stats));

    const joinMetrics = stats[0].metrics.supplementalMetrics.JoinOptimization;
    assert(joinMetrics);

    assert.eq(joinMetrics.updateCount, 2, tojson(joinMetrics));
}

MongoRunner.stopMongod(conn);
