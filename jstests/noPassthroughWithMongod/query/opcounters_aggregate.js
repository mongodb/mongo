/**
 * Tests the serverStatus opcounters for the 'aggregate' command.
 *
 * As of SERVER-123987, aggregate has its own dedicated opcounters.aggregates counter and no longer
 * increments opcounters.queries (its pre-SERVER-123987 behavior) or opcounters.commands.
 *
 * Lives in noPassthroughWithMongod so the harness provides a standalone MongoDFixture with no
 * background hooks, allowing exact-delta assertions on the aggregate counter.
 *
 * See jstests/sharding/query/opcounters_aggregate.js for sharded-cluster coverage.
 *
 * @tags: [
 *   # The config fuzzer may run logical session cache refreshes in the background, which modifies
 *   # some serverStatus metrics read in this test.
 *   does_not_support_config_fuzzer,
 *   # The aggregate opcounters were added for 9.0.
 *   requires_fcv_90,
 * ]
 */

const coll = db.getCollection(jsTestName());
coll.drop();
assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));

function getCounters() {
    return db.serverStatus().opcounters;
}

// aggregate increments opcounters.aggregates (not queries). Exact-delta assertions are safe here
// since no background processes fire this counter.

let before = getCounters();
coll.aggregate([{$match: {}}]).toArray();
let after = getCounters();
assert.eq(before.aggregate + 1, after.aggregate, "aggregate counter should increment by 1");
assert.eq(before.query, after.query, "aggregate should NOT increment queries counter");

// Each pipeline invocation counts as one, regardless of stage count.
before = getCounters();
coll.aggregate([{$match: {a: {$gt: 1}}}]).toArray();
coll.aggregate([{$group: {_id: null, total: {$sum: "$a"}}}]).toArray();
after = getCounters();
assert.eq(before.aggregate + 2, after.aggregate, "two aggregates should increment counter by 2");

// find increments opcounters.queries, not opcounters.aggregates.

before = getCounters();
coll.find({}).toArray();
after = getCounters();
assert.eq(before.query + 1, after.query, "find should increment queries counter");
assert.eq(before.aggregate, after.aggregate, "find should NOT increment aggregate counter");

// A failed aggregate still increments the counter (fired before execution).

before = getCounters();
assert.commandFailed(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$match: {$expr: {$invalidOp: []}}}],
        cursor: {},
    }),
);
after = getCounters();
assert.eq(before.aggregate + 1, after.aggregate, "failed aggregate should still increment counter");
assert.eq(before.query, after.query, "failed aggregate should NOT increment queries counter");
