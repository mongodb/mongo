/**
 * Tests that the 'persistentStatsConfig' collection option is kept when a collection is sharded,
 * when its chunks are migrated to another shard and when it is resharded.
 *
 * @tags: [
 *   featureFlagPersistentStatsCatalog,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {readPersistentStatsConfig} from "jstests/libs/query/persistent_samples_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const config = {samples: {sampleSize: 100, joinSampleSize: 200}};

describe("persistentStatsConfig on a sharded cluster", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            other: {
                configOptions: {setParameter: {reshardingMinimumOperationDurationMillis: 0}},
                rsOptions: {setParameter: {reshardingMinimumOperationDurationMillis: 0}},
            },
        });
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        this.db = this.st.s.getDB(dbName);

        this.configOnShards = (collName) =>
            [this.st.rs0, this.st.rs1].map((rs) =>
                readPersistentStatsConfig(rs.getPrimary().getDB(dbName), collName),
            );

        // Shards the collection on '_id' and leaves one chunk on each shard.
        this.shardAndSplit = (collName) => {
            const ns = dbName + "." + collName;
            assert.commandWorked(
                this.db[collName].insert([
                    {_id: -5, x: 1},
                    {_id: 5, x: 2},
                ]),
            );
            assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
            assert.commandWorked(this.st.s.adminCommand({split: ns, middle: {_id: 0}}));
            assert.commandWorked(
                this.st.s.adminCommand({
                    moveChunk: ns,
                    find: {_id: 5},
                    to: this.st.shard1.shardName,
                    _waitForDelete: true,
                }),
            );
        };
    });

    after(function () {
        this.st.stop();
    });

    it("is kept when a collection that was modified is sharded and a chunk is moved", function () {
        const collName = "collMod";
        assert.commandWorked(this.db.createCollection(collName));
        assert.commandWorked(
            this.db.runCommand({collMod: collName, persistentStatsConfig: config}),
        );
        assert.docEq(readPersistentStatsConfig(this.db, collName), config);

        this.shardAndSplit(collName);

        assert.docEq(this.configOnShards(collName), [config, config]);
        assert.docEq(readPersistentStatsConfig(this.db, collName), config);
    });

    it("is kept when a collection created with it is sharded and a chunk is moved", function () {
        const collName = "create";
        assert.commandWorked(this.db.createCollection(collName, {persistentStatsConfig: config}));
        assert.docEq(readPersistentStatsConfig(this.db, collName), config);

        this.shardAndSplit(collName);

        assert.docEq(this.configOnShards(collName), [config, config]);
    });

    it("applies a collMod delta on every shard of a sharded collection", function () {
        const collName = "collMod";
        assert.commandWorked(
            this.db.runCommand({
                collMod: collName,
                persistentStatsConfig: {samples: {sampleSize: null}},
            }),
        );

        const expected = {samples: {joinSampleSize: 200}};
        assert.docEq(this.configOnShards(collName), [expected, expected]);
        assert.docEq(readPersistentStatsConfig(this.db, collName), expected);
    });

    it("is not applied by shardCollection on a non-existing namespace", function () {
        const collName = "shardCollection";
        assert.commandWorked(
            this.st.s.adminCommand({
                shardCollection: dbName + "." + collName,
                key: {_id: 1},
                persistentStatsConfig: config,
            }),
        );
        assert.eq(undefined, readPersistentStatsConfig(this.db, collName));
    });

    it("is kept when the collection is resharded", function () {
        const collName = "create";
        assert.commandWorked(
            this.st.s.adminCommand({
                reshardCollection: dbName + "." + collName,
                key: {x: 1},
                numInitialChunks: 2,
            }),
        );

        // Resharding may leave a shard without any chunk, in which case it no longer has the
        // collection. Every shard that does have it must keep the option.
        const infos = [this.st.rs0, this.st.rs1].flatMap((rs) =>
            rs.getPrimary().getDB(dbName).getCollectionInfos({name: collName}),
        );
        assert.gte(infos.length, 1, "expected at least one shard to own the collection");
        for (const info of infos) {
            assert.docEq(info.options.persistentStatsConfig, config, "shard lost the option", {
                info,
            });
        }
        assert.docEq(readPersistentStatsConfig(this.db, collName), config);
    });
});
