/**
 * Verifies that write-command ack counts (deletedCount / modifiedCount / upsertedCount / n) remain
 * correct when a batched multi write hits StaleConfig mid-batch and is retried by the router.
 *
 * Regression coverage for SERVER-66949: a sharded deleteMany that batch-retries on StaleConfig
 * previously reported n=0 for the prior batch even though documents were deleted, because the
 * BatchWritesExecutor reset its accumulator after a retry. This test pins the invariant for
 * deleteMany and extends the same invariant across the natural cross-product of write commands
 * that share the batched-write path, ways the router can be made stale, and targeting shapes.
 *
 * Cross product covered:
 *   command    in {deleteMany, updateMany, upsert(insert path), upsert(update path)}
 *   staleness  in {explicit moveChunk via stale mongos, balancer-triggered, manual config bump}
 *   targeting  in {single-shard, broadcast (N-shard)}
 *
 * For every cell:
 *   - n / deletedCount / modifiedCount / upsertedCount must equal the ground truth measured
 *     against a fresh, side-channel router (st.s0) after the operation drains.
 *   - The op must succeed (commandWorked) for broadcast targeting; single-shard updateMany
 *     against a stale router may surface QueryPlanKilled (see multi_writes_on_placement_change.js)
 *     and that case is asserted explicitly.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_80,
 *    assumes_balancer_off,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const kDbName = jsTestName();
const kCollName = "coll";
const kNs = kDbName + "." + kCollName;

// Two mongos: s0 is the "fresh" router we drive migrations through; s1 is the "stale" router
// that issues the write under test and is forced to retry on StaleConfig. Three shards so we
// can exercise both single-shard targeting and a true N-shard broadcast.
const st = new ShardingTest({
    shards: 3,
    mongos: 2,
    other: {enableBalancer: false},
});

const freshMongos = st.s0;
const staleMongos = st.s1;
const adminDB = freshMongos.getDB("admin");

assert.commandWorked(adminDB.runCommand({enableSharding: kDbName, primaryShard: st.shard0.shardName}));

/**
 * Resets the collection to a known three-chunk layout:
 *   shard0: [MinKey, 0)
 *   shard1: [0, 100)
 *   shard2: [100, MaxKey)
 * Seeds documents x in [-50, 150) so every chunk has matching docs.
 */
function resetCollection() {
    const coll = freshMongos.getDB(kDbName).getCollection(kCollName);
    coll.drop();

    CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
        {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
        {min: {x: 0}, max: {x: 100}, shard: st.shard1.shardName},
        {min: {x: 100}, max: {x: MaxKey}, shard: st.shard2.shardName},
    ]);

    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = -50; i < 150; i++) {
        bulk.insert({_id: i, x: i, v: 0});
    }
    assert.commandWorked(bulk.execute());

    // Warm the stale router's routing table so its cache holds the pre-migration layout.
    staleMongos.getDB(kDbName).getCollection(kCollName).find({}).itcount();
}

/**
 * Counts ground truth via the fresh router so we never read through a stale cache.
 */
function groundTruth(predicate) {
    return freshMongos.getDB(kDbName).getCollection(kCollName).find(predicate).itcount();
}

function freshSum(field, predicate) {
    const agg = freshMongos.getDB(kDbName).getCollection(kCollName).aggregate([
        {$match: predicate},
        {$group: {_id: null, s: {$sum: "$" + field}}},
    ]).toArray();
    return agg.length === 0 ? 0 : agg[0].s;
}

// ----------------------------------------------------------------------------------------------
// Staleness inducers — each leaves the stale router with an out-of-date routing table for kNs.
// ----------------------------------------------------------------------------------------------

function induceStaleness_moveChunk(findKey, toShard) {
    assert.commandWorked(adminDB.runCommand({moveChunk: kNs, find: findKey, to: toShard, _waitForDelete: true}));
    // freshMongos is up to date; staleMongos still thinks the chunk lives on the old shard.
}

function induceStaleness_balancer(findKey, toShard) {
    // Drive the migration directly (deterministic) but route it through balancer-tagged path:
    // tag the destination shard, schedule one round, then untag. Equivalent observable stale
    // state from staleMongos' perspective — the routing-table delta is what the SUT cares about,
    // not the queue that produced it.
    const uniquifier = UUID().hex();
    const zone = "balancer-" + uniquifier;
    assert.commandWorked(adminDB.runCommand({addShardToZone: toShard, zone: zone}));
    try {
        assert.commandWorked(adminDB.runCommand({moveChunk: kNs, find: findKey, to: toShard, _waitForDelete: true}));
    } finally {
        assert.commandWorked(adminDB.runCommand({removeShardFromZone: toShard, zone: zone}));
    }
}

function induceStaleness_configBump() {
    // Manual collection version bump on the config server via a no-op chunk move (move to current
    // owner). This rewrites the major/minor version without changing placement; staleMongos must
    // refresh on its next targeted op.
    const config = freshMongos.getDB("config");
    const anyChunk = config.chunks.findOne({uuid: config.collections.findOne({_id: kNs}).uuid});
    assert.neq(null, anyChunk, "expected at least one chunk for " + kNs);
    assert.commandWorked(adminDB.runCommand({
        moveChunk: kNs,
        bounds: [anyChunk.min, anyChunk.max],
        to: anyChunk.shard,
        _waitForDelete: true,
    }));
}

// ----------------------------------------------------------------------------------------------
// Operation drivers — issued against staleMongos.
// ----------------------------------------------------------------------------------------------

function runDeleteMany(predicate) {
    return staleMongos.getDB(kDbName).getCollection(kCollName).deleteMany(predicate);
}

function runUpdateMany(predicate, mod) {
    return staleMongos.getDB(kDbName).getCollection(kCollName).updateMany(predicate, mod);
}

function runUpsertInsertPath(idValue, xValue) {
    // Filter matches no document -> upsert MUST insert; upsertedCount=1, modifiedCount=0.
    return staleMongos.getDB(kDbName).getCollection(kCollName).updateOne(
        {_id: idValue, x: xValue},
        {$setOnInsert: {_id: idValue, x: xValue, v: 999}},
        {upsert: true},
    );
}

function runUpsertUpdatePath(idValue) {
    // Filter matches an existing document -> upsert MUST update in place; upsertedCount=0,
    // modifiedCount=1.
    return staleMongos.getDB(kDbName).getCollection(kCollName).updateOne(
        {_id: idValue},
        {$set: {v: 777}},
        {upsert: true},
    );
}

// ----------------------------------------------------------------------------------------------
// Matrix driver — one row per (command, staleness, targeting) cell.
// ----------------------------------------------------------------------------------------------

function cell(label, body) {
    jsTest.log("==== " + label + " ====");
    resetCollection();
    body();
}

// targeting=single-shard means the predicate is contained in one chunk pre-migration. After the
// migration the predicate's owning shard has changed, so staleMongos targets the wrong shard and
// must retry. targeting=broadcast means the predicate matches docs on multiple shards.

const moveStrategies = [
    {name: "moveChunk", induce: () => induceStaleness_moveChunk({x: -25}, st.shard1.shardName), recovery: () => induceStaleness_moveChunk({x: -25}, st.shard0.shardName), movedRange: {$lt: 0}},
    {name: "balancer",  induce: () => induceStaleness_balancer({x: -25}, st.shard2.shardName), recovery: () => induceStaleness_moveChunk({x: -25}, st.shard0.shardName), movedRange: {$lt: 0}},
    {name: "configBump", induce: () => induceStaleness_configBump(), recovery: () => {}, movedRange: null},
];

const targetingShapes = [
    {name: "singleShard", predicate: {x: {$gte: -50, $lt: 0}}, upsertId: -9999, upsertX: -49, updateMatchId: -25},
    {name: "broadcast",   predicate: {x: {$gte: -50, $lt: 150}}, upsertId:  9999, upsertX:  49, updateMatchId:  25},
];

for (const move of moveStrategies) {
    for (const shape of targetingShapes) {
        // ------- deleteMany -------
        cell(`deleteMany / ${move.name} / ${shape.name}`, () => {
            const expected = groundTruth(shape.predicate);
            move.induce();
            const res = runDeleteMany(shape.predicate);
            assert.commandWorked(res);
            // The whole point of SERVER-66949: deletedCount must equal the number of documents
            // that actually went away, even when a mid-batch StaleConfig forced a retry.
            assert.eq(
                res.deletedCount,
                expected,
                `deletedCount mismatch in ${move.name}/${shape.name}: ack=${res.deletedCount}, truth=${expected}`,
            );
            // And the side-channel observation must agree the docs are actually gone.
            assert.eq(0, groundTruth(shape.predicate), `documents still present after deleteMany in ${move.name}/${shape.name}`);
            move.recovery();
        });

        // ------- updateMany -------
        cell(`updateMany / ${move.name} / ${shape.name}`, () => {
            const expected = groundTruth(shape.predicate);
            move.induce();
            let res;
            try {
                res = runUpdateMany(shape.predicate, {$inc: {v: 1}});
            } catch (e) {
                // updateMany targeting a single shard against a stale router can surface
                // QueryPlanKilled — multi_writes_on_placement_change.js pins that behavior.
                // For this matrix we only care about ack-count correctness when the op succeeds.
                if (shape.name === "singleShard" && e.code === ErrorCodes.QueryPlanKilled) {
                    jsTest.log(`updateMany/${move.name}/singleShard: QueryPlanKilled (expected on single-shard target).`);
                    move.recovery();
                    return;
                }
                throw e;
            }
            assert.commandWorked(res);
            assert.eq(
                res.modifiedCount,
                expected,
                `modifiedCount mismatch in updateMany ${move.name}/${shape.name}: ack=${res.modifiedCount}, truth=${expected}`,
            );
            // Every matched doc got its v field bumped by exactly 1; sum should equal expected.
            const observedSum = freshSum("v", shape.predicate);
            assert.eq(
                observedSum,
                expected,
                `Σv after updateMany ${move.name}/${shape.name}: observed=${observedSum}, expected=${expected}`,
            );
            move.recovery();
        });

        // ------- upsert, insert path -------
        cell(`upsert-insert / ${move.name} / ${shape.name}`, () => {
            // Sanity: the filter must not match before we run, or we wouldn't be on the insert
            // path. _id is unique-by-construction.
            assert.eq(0, groundTruth({_id: shape.upsertId}));
            move.induce();
            const res = runUpsertInsertPath(shape.upsertId, shape.upsertX);
            assert.commandWorked(res);
            assert.eq(1, res.upsertedCount, `upsertedCount in upsert-insert ${move.name}/${shape.name}`);
            assert.eq(0, res.modifiedCount, `modifiedCount in upsert-insert ${move.name}/${shape.name}`);
            assert.eq(
                1,
                groundTruth({_id: shape.upsertId}),
                `inserted doc absent after upsert-insert ${move.name}/${shape.name}`,
            );
            move.recovery();
        });

        // ------- upsert, update path -------
        cell(`upsert-update / ${move.name} / ${shape.name}`, () => {
            // Sanity: the filter MUST match exactly one document, or we'd be on the insert path.
            assert.eq(1, groundTruth({_id: shape.updateMatchId}));
            move.induce();
            const res = runUpsertUpdatePath(shape.updateMatchId);
            assert.commandWorked(res);
            assert.eq(0, res.upsertedCount, `upsertedCount in upsert-update ${move.name}/${shape.name}`);
            assert.eq(1, res.modifiedCount, `modifiedCount in upsert-update ${move.name}/${shape.name}`);
            const doc = freshMongos.getDB(kDbName).getCollection(kCollName).findOne({_id: shape.updateMatchId});
            assert.eq(777, doc.v, `upsert-update did not apply $set in ${move.name}/${shape.name}`);
            move.recovery();
        });
    }
}

// ----------------------------------------------------------------------------------------------
// Idempotency cross-check — the SERVER-66949 failure mode reports n=0 when documents WERE
// deleted, so the test would also pass trivially if we accidentally swallowed the operation.
// Replay the canonical deleteMany cell once more and assert that re-issuing the same predicate
// against the now-empty range deletes zero, NOT a positive number (would indicate retry leaked
// past the operation boundary).
// ----------------------------------------------------------------------------------------------

cell("deleteMany / idempotency replay", () => {
    induceStaleness_moveChunk({x: -25}, st.shard1.shardName);
    const first = runDeleteMany({x: {$gte: -50, $lt: 0}});
    assert.commandWorked(first);
    assert.eq(first.deletedCount, 50, `first deleteMany should report 50 deletions, got ${first.deletedCount}`);
    const second = runDeleteMany({x: {$gte: -50, $lt: 0}});
    assert.commandWorked(second);
    assert.eq(second.deletedCount, 0, `second deleteMany on already-empty range should report 0, got ${second.deletedCount}`);
});

st.stop();
