/**
 * Verifies that MoveChunkCommand produces the correct V2 change stream events
 * (normal + system) when draining all chunks off the primary shard.
 *
 * Four scenarios (range × empty/non-empty, hashed × empty/non-empty):
 *   - Empty collection: shard → moveChunk (inserts internally) → unshard
 *   - Non-empty collection: insert → shard → moveChunk → unshard
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   featureFlagChangeStreamPreciseShardTargeting,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {
    CreateDatabaseCommand,
    CreateIndexCommand,
    InsertDocCommand,
    ShardCollectionCommand,
    MoveChunkCommand,
    UnshardCollectionCommand,
    getShardKeySpec,
    ShardingType,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {createShardingTest} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {ChangeStreamTest, ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

const ignoredEventTypes = ["createIndexes", "dropIndexes", "startIndexBuild", "commitIndexBuild"];

function buildCommands({dbName, collName, shards, shardingType, nonEmpty}) {
    const shardKey = getShardKeySpec(shardingType);
    const commands = [];
    let ctx = {exists: false, nonEmpty: false, shardKeySpec: null, isUnsplittable: false};

    commands.push(new CreateDatabaseCommand(dbName, collName, shards, ctx));

    if (nonEmpty) {
        commands.push(new InsertDocCommand(dbName, collName, shards, ctx));
        ctx = {exists: true, nonEmpty: true, shardKeySpec: null, isUnsplittable: false};
        commands.push(new CreateIndexCommand(dbName, collName, shards, ctx, shardKey));
    }

    commands.push(new ShardCollectionCommand(dbName, collName, shards, ctx, shardKey));

    const shardedCtx = {exists: true, nonEmpty, shardKeySpec: shardKey, isUnsplittable: false};
    commands.push(new MoveChunkCommand(dbName, collName, shards, shardedCtx));
    commands.push(new UnshardCollectionCommand(dbName, collName, shards, shardedCtx));

    return commands;
}

function runMoveChunkTest({mongos, shards, dbName, collName, shardingType, nonEmpty, expectedTypes}) {
    const db = mongos.getDB(dbName);
    assert.commandWorked(db.dropDatabase());

    const csTest = new ChangeStreamTest(db);
    let cursor = csTest.startWatchingChanges({
        pipeline: [
            {$changeStream: {version: "v2", showExpandedEvents: true, showSystemEvents: true}},
            {$match: {operationType: {$nin: ignoredEventTypes}}},
        ],
        collection: collName,
        aggregateOptions: {cursor: {batchSize: 0}},
    });

    const commands = buildCommands({dbName, collName, shards, shardingType, nonEmpty});
    for (const cmd of commands) {
        cmd.execute(mongos);
    }

    const events = csTest.getNextChanges(cursor, expectedTypes.length, false);
    const actualTypes = events.map((e) => e.operationType);
    jsTest.log.info("Collected events", {shardingType, nonEmpty, types: actualTypes});

    assert.eq(actualTypes, expectedTypes, `Event mismatch for ${shardingType} (nonEmpty=${nonEmpty})`);
    csTest.assertNoChange(cursor);

    csTest.cleanUp();
}

describe("MoveChunk System Events (v2)", function () {
    const dbName = "test_move_chunk_events";
    const collName = "coll";

    before(function () {
        this.st = createShardingTest(/* mongos */ 1, /* shards */ 3);
        this.st.stopBalancer();
        this.shards = assert.commandWorked(this.st.s.adminCommand({listShards: 1})).shards;
    });

    after(function () {
        this.st.s.getDB(dbName).dropDatabase();
        this.st.stop();
    });

    // MoveChunkCommand inserts 4 initial docs (max(shards+1, numDocs)), moves chunks
    // (system events interleaved), then inserts 1 interleaved doc after the first move.
    // UnshardCollectionCommand follows.
    //
    // Range and hashed non-empty share the same event pattern; only the collection setup
    // prefix differs (empty starts with create+shardCollection; non-empty adds 1 insert
    // before shardCollection).
    // Hashed empty is different because pre-splitting distributes chunks at creation,
    // leaving the donor with fewer chunks to drain.
    const emptyPrefix = ["create", "shardCollection"];
    const nonEmptyPrefix = ["create", "insert", "shardCollection"];

    const defaultMoveAndUnshard = [
        "insert",
        "insert",
        "insert",
        "insert",
        "moveChunk",
        "insert",
        "moveChunk",
        "migrateLastChunkFromShard",
        "moveChunk",
        "reshardBlockingWrites",
        "reshardBlockingWrites",
        "reshardCollection",
    ];

    const hashedEmptyMoveAndUnshard = [
        "insert",
        "insert",
        "insert",
        "insert",
        "migrateLastChunkFromShard",
        "moveChunk",
        "insert",
        "reshardBlockingWrites",
        "reshardBlockingWrites",
        "reshardCollection",
    ];

    it("range, empty collection", function () {
        runMoveChunkTest({
            mongos: this.st.s,
            shards: this.shards,
            dbName,
            collName,
            shardingType: ShardingType.RANGE,
            nonEmpty: false,
            expectedTypes: [...emptyPrefix, ...defaultMoveAndUnshard],
        });
    });

    it("range, non-empty collection", function () {
        runMoveChunkTest({
            mongos: this.st.s,
            shards: this.shards,
            dbName,
            collName,
            shardingType: ShardingType.RANGE,
            nonEmpty: true,
            expectedTypes: [...nonEmptyPrefix, ...defaultMoveAndUnshard],
        });
    });

    it("hashed, empty collection", function () {
        runMoveChunkTest({
            mongos: this.st.s,
            shards: this.shards,
            dbName,
            collName,
            shardingType: ShardingType.HASHED,
            nonEmpty: false,
            expectedTypes: [...emptyPrefix, ...hashedEmptyMoveAndUnshard],
        });
    });

    it("hashed, non-empty collection", function () {
        runMoveChunkTest({
            mongos: this.st.s,
            shards: this.shards,
            dbName,
            collName,
            shardingType: ShardingType.HASHED,
            nonEmpty: true,
            expectedTypes: [...nonEmptyPrefix, ...defaultMoveAndUnshard],
        });
    });
});
