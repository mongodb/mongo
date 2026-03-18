/**
 * Test that getShardVersion and getDatabaseVersion correctly support the "latestCached" option,
 * returning cached information without triggering a refresh and including timeInStore.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("getShardVersion and getDatabaseVersion latestCached", function () {
    let st, dbName, collName, ns, shardConn;

    before(function () {
        st = new ShardingTest({shards: 1});
        dbName = jsTestName();
        collName = "foo";
        ns = dbName + "." + collName;
        shardConn = st.rs0.getPrimary();

        assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({x: 10}));
    });

    after(function () {
        st.stop();
    });

    describe("mongos", function () {
        it("with latestCached: true returns timeInStore and same version", function () {
            const latestCachedRes = assert.commandWorked(st.s.adminCommand({getShardVersion: ns, latestCached: true}));
            assert.neq(undefined, latestCachedRes.timeInStore);
            assert.eq(undefined, latestCachedRes.chunks);
        });

        it("with latestCached and fullMetadata returns chunks and timeInStore", function () {
            const latestCachedRes = assert.commandWorked(
                st.s.adminCommand({getShardVersion: ns, latestCached: true, fullMetadata: true}),
            );
            assert.neq(undefined, latestCachedRes.timeInStore);
            assert.eq(1, latestCachedRes.chunks.length);
            assert.eq(latestCachedRes.chunks[0][0].x, MinKey);
            assert.eq(latestCachedRes.chunks[0][1].x, MaxKey);
        });

        it("with latestCached for database returns primaryShard, dbVersion, timeInStore", function () {
            const dbOnlyRes = assert.commandWorked(st.s.adminCommand({getDatabaseVersion: dbName, latestCached: true}));
            assert.neq(undefined, dbOnlyRes.primaryShard);
            assert.neq(undefined, dbOnlyRes.dbVersion);
            assert.neq(undefined, dbOnlyRes.timeInStore);
        });
    });

    describe("mongod", function () {
        it("with latestCached: true returns routing info with timeInStore", function () {
            const shardRes = assert.commandWorked(shardConn.adminCommand({getShardVersion: ns, latestCached: true}));
            assert.neq(undefined, shardRes.version);
            assert.neq(undefined, shardRes.versionEpoch);
            assert.neq(undefined, shardRes.timeInStore);
        });

        it("with latestCached for database returns primaryShard, dbVersion, timeInStore", function () {
            // Now that shards are authoritative for the database, the catalog cache rarely has the
            // database metadata so the result is likely to be UNKNOWN.
            const shardRes = assert.commandWorked(
                shardConn.adminCommand({getDatabaseVersion: dbName, latestCached: true}),
            );
            if (shardRes.global === "UNKNOWN") {
                assert.eq(undefined, shardRes.primaryShard);
                assert.eq(undefined, shardRes.dbVersion);
                assert.eq(undefined, shardRes.timeInStore);
            } else {
                assert.neq(undefined, shardRes.primaryShard);
                assert.neq(undefined, shardRes.dbVersion);
                assert.neq(undefined, shardRes.timeInStore);
            }
        });
    });
});
