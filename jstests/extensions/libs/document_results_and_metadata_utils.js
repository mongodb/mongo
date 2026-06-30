import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

/**
 * A minimal well-formed metadata object used as a default in tests that don't care about specific
 * metadata values.
 */
export const kSimpleExpectedMeta = {count: {lowerBound: 42}};

/**
 * Drops and recreates `coll` in DRM-compatible form: hashed-sharded on mongos, or a single seeded
 * document on standalone so that numberOfShardsForCollection returns 1.
 */
export function setupDrmCollection(db, coll) {
    assertDropCollection(db, coll.getName());
    if (FixtureHelpers.isMongos(db)) {
        assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
        assert.commandWorked(
            db.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
        );
    } else {
        assert.commandWorked(coll.insertOne({_id: 0}));
    }
}

/**
 * Returns {nShards, shardIds, isSharded} for the given collection. shardIds is sorted for
 * deterministic per-shard metadata assignment; empty on standalone.
 */
export function getDrmShardInfo(db, coll) {
    return {
        nShards: FixtureHelpers.numberOfShardsForCollection(coll),
        shardIds: FixtureHelpers.isMongos(db)
            ? FixtureHelpers.getShardsOwningDataForCollection(coll).slice().sort()
            : [],
        isSharded: FixtureHelpers.isSharded(coll),
    };
}

/**
 * Repeats the full docs list once per shard: [A,B] × 2 → [A,B, A,B]. Use when results are
 * collected without a global merge sort (each shard emits all its docs independently).
 */
export function expandPerShard(nShards, docs) {
    return Array(nShards).fill(docs).flat();
}

/**
 * Repeats each doc once per shard: [A,B] × 2 → [A,A, B,B]. Use after a global $sort where the
 * merger interleaves one doc at a time across shards.
 */
export function expandEachPerShard(nShards, docs) {
    return docs.flatMap((d) => Array(nShards).fill(d));
}

/**
 * Builds a two-shard byShard map (lowerBound 10 + 32 = 42) and a merge pipeline that sums
 * count.lowerBound across shards. Returns {byShard, mergePipeline, expectedMeta} where expectedMeta
 * is kSimpleExpectedMeta, the merged result.
 */
export function makeCountMergeSetup(shardIds) {
    return {
        byShard: {
            [shardIds[0]]: {meta: {count: {lowerBound: 10}}},
            [shardIds[1]]: {meta: {count: {lowerBound: 32}}},
        },
        mergePipeline: [
            {$group: {_id: null, count: {$sum: "$count.lowerBound"}}},
            {$project: {_id: 0, count: {lowerBound: "$count"}}},
        ],
        expectedMeta: kSimpleExpectedMeta,
    };
}
