/**
 * Verifies that the legacy non-authoritative shard-local cache collections (config.cache.*) are
 * dropped when upgrading to the FCV where the authoritative shard catalog takes over, and that a
 * downgrade cleans up any such collections that may have remained on the upgraded FCV.
 *
 * TODO (SERVER-98118): Remove this test once 9.0 becomes last LTS.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function listCacheCollections(shardConn) {
    return shardConn
        .getDB("config")
        .getCollectionNames()
        .filter((name) => name.startsWith("cache."));
}

function assertNoCacheCollections(shardConn, shardName) {
    assert.eq(
        [],
        listCacheCollections(shardConn),
        `Unexpected config.cache.* collections on ${shardName}`,
    );
}

describe("legacy cache collections on FCV transitions", function () {
    it("drops config.cache.* on upgrade and cleans up leftovers on downgrade", function () {
        const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

        if (
            lastLTSFCV !== "8.0" ||
            !FeatureFlagUtil.isPresentAndEnabled(st.s, "AuthoritativeShardsCRUD")
        ) {
            jsTestLog("Skipping: not running across the FCV transition to Authoritative Shards");
            st.stop();
            return;
        }

        const shards = [
            {name: st.shard0.shardName, conn: st.rs0.getPrimary()},
            {name: st.shard1.shardName, conn: st.rs1.getPrimary()},
        ];

        // Upgrade: legacy config.cache.* get dropped.

        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        const dbName = "test";
        const coll = `${dbName}.sharded`;
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: shards[0].name}),
        );
        assert.commandWorked(st.s.adminCommand({shardCollection: coll, key: {x: 1}}));
        assert.commandWorked(st.s.adminCommand({split: coll, middle: {x: 0}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: coll, find: {x: 0}, to: shards[1].name}),
        );
        assert.commandWorked(st.s.getCollection(coll).insert([{x: -1}, {x: 1}]));
        // Reading persists cache.collections + cache.chunks.* on each shard.
        assert.eq(2, st.s.getCollection(coll).find().itcount());
        // A read only persists cache.databases on the db-primary, so flush it on the non-primary too.
        for (const {conn} of shards) {
            assert.commandWorked(
                conn.adminCommand({_flushDatabaseCacheUpdates: dbName, syncFromConfig: true}),
            );
        }
        for (const {name, conn} of shards) {
            const cacheCollections = listCacheCollections(conn);
            for (const expected of [
                "cache.databases",
                "cache.collections",
                `cache.chunks.${coll}`,
            ]) {
                assert.contains(
                    expected,
                    cacheCollections,
                    `expected ${expected} on ${name} at lastLTSFCV`,
                );
            }
        }

        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        for (const {name, conn} of shards) {
            assertNoCacheCollections(conn, `${name} after upgrade`);
        }

        // Downgrade: leftover config.cache.* (e.g. from an unclean upgrade) get cleaned up.

        const leftoverNss = `${dbName}.leftover`;
        const leftoverEntry = {
            _id: leftoverNss,
            uuid: UUID(),
            key: {x: 1},
            unique: false,
            epoch: new ObjectId(),
            timestamp: Timestamp(1, 1),
        };
        for (const shard of shards) {
            const db = shard.conn.getDB("config");
            assert.commandWorked(db.getCollection("cache.databases").insert({_id: dbName}));
            assert.commandWorked(db.getCollection("cache.collections").insert(leftoverEntry));
            assert.commandWorked(db.createCollection(`cache.chunks.${leftoverNss}`));

            // Check by UUID so that we detect the drop even if they get re-created after downgrading the FCV.
            shard.leftoverUUIDs = [
                db.getCollection("cache.databases").getUUID(),
                db.getCollection("cache.collections").getUUID(),
                db.getCollection(`cache.chunks.${leftoverNss}`).getUUID(),
            ];
        }

        assert.commandWorked(
            st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        for (const {name, conn, leftoverUUIDs} of shards) {
            assert.eq(
                [],
                conn.getDB("config").getCollectionInfos({"info.uuid": {$in: leftoverUUIDs}}),
                `Leftover cache collections not dropped on ${name}`,
            );
        }

        st.stop();
    });
});
