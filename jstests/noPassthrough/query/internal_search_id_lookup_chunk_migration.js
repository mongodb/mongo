/**
 * Tests that $_internalSearchIdLookup has shard filtering applied to it during a chunk migration,
 * so that orphaned documents left behind by the migration are not returned.
 *
 * `$_internalSearchIdLookup` resolves `_id`s through one of two executors depending on the
 * `featureFlagSearchOptimizedIdLookup` IFR flag: the Express fast path (flag on) or the classic
 * local-read path (flag off). Because the flag is toggleable at runtime, we run the suite once per
 * state so orphan filtering is exercised on both executors.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const dbName = "test";
const collName = "internal_search_id_lookup_chunk_migration";

// Runs an aggregate directly against a shard on the internal client connection, attaching the
// given shardVersion so the shard applies its normal ownership/orphan filtering to the read (the
// same filtering it would apply if the request had been routed through mongos).
// $_internalSearchIdLookup is an internal-only stage: it is rejected in user requests and only
// accepted from an internal client. internalClient connections must specify an explicit
// writeConcern on every command that accepts one.
function idLookupAgg(internalDB, collectionName, pipeline, shardVersion) {
    const res = assert.commandWorked(
        internalDB.runCommand({
            aggregate: collectionName,
            pipeline: pipeline,
            cursor: {},
            shardVersion: shardVersion,
            readConcern: {},
            writeConcern: {},
        }),
    );
    return new DBCommandCursor(internalDB, res);
}

// Opens an internal-client connection directly to a shard, bypassing mongos, so that
// $_internalSearchIdLookup (an internal-only stage) can be run against it.
function openInternalConn(shardConn) {
    const internalConn = new Mongo(shardConn.host);
    assert.commandWorked(
        internalConn.getDB("admin").runCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
        }),
    );
    return internalConn;
}

// TODO SERVER-130493 Remove optimizedIdLookup when no-ff search variant exists.
for (const optimizedIdLookup of [false, true]) {
    describe(
        `$_internalSearchIdLookup shard filtering during chunk migration ` +
            `(featureFlagSearchOptimizedIdLookup=${optimizedIdLookup})`,
        function () {
            before(function () {
                // This test deliberately leaves orphans behind on the donor shard by suspending
                // range deletion, so the orphan-checking hook must be disabled.
                TestData.skipCheckOrphans = true;

                this.st = new ShardingTest({
                    shards: {
                        rs0: {
                            nodes: 1,
                            setParameter: {featureFlagSearchOptimizedIdLookup: optimizedIdLookup},
                        },
                        rs1: {
                            nodes: 1,
                            setParameter: {featureFlagSearchOptimizedIdLookup: optimizedIdLookup},
                        },
                    },
                    mongos: 1,
                });

                const mongos = this.st.s;
                this.testDB = mongos.getDB(dbName);
                this.testColl = this.testDB.getCollection(collName);
                this.collNS = this.testColl.getFullName();

                assert.commandWorked(
                    this.testDB.adminCommand({
                        enableSharding: dbName,
                        primaryShard: this.st.shard0.shardName,
                    }),
                );

                // Documents that will remain on shard0 for the whole test.
                for (const id of [1, 2, 3, 4]) {
                    assert.commandWorked(this.testColl.insert({_id: id, shardKey: id}));
                }
                // Documents that will migrate from shard0 to shard1 during the test.
                for (const id of [15, 16, 17, 18]) {
                    assert.commandWorked(this.testColl.insert({_id: id, shardKey: id}));
                }
                // Documents that already live on shard1 before the migration.
                for (const id of [101, 102, 103, 104]) {
                    assert.commandWorked(this.testColl.insert({_id: id, shardKey: id}));
                }

                assert.commandWorked(this.testColl.createIndex({shardKey: 1}));
                // Shard the collection and move the [100, maxKey) chunk to shard1, then split
                // shard0's remaining chunk so that only [10, 100) will migrate during the test.
                //   shard0: [minKey, 10)   - docs 1-4
                //   shard0: [10, 100)      - docs 15-18 (will be migrated during the test)
                //   shard1: [100, maxKey)  - docs 101-104
                this.st.shardColl(
                    this.testColl,
                    {shardKey: 1},
                    {shardKey: 100},
                    {shardKey: 100},
                    dbName,
                    true /* waitForDelete */,
                );
                assert.commandWorked(
                    this.testDB.adminCommand({split: this.collNS, middle: {shardKey: 10}}),
                );

                this.shard0Conn = this.st.rs0.getPrimary();
                this.shard1Conn = this.st.rs1.getPrimary();
                this.shard0Coll = this.shard0Conn.getDB(dbName)[collName];

                this.internalConn0 = openInternalConn(this.shard0Conn);
                this.internalConn1 = openInternalConn(this.shard1Conn);
                this.internalDB0 = this.internalConn0.getDB(dbName);
                this.internalDB1 = this.internalConn1.getDB(dbName);

                // Pause range deletion on shard0 (the donor) so that once we migrate the [10, 100)
                // chunk away, the physically-present documents become real orphans instead of being
                // cleaned up.
                this.suspendRangeDeletionFp = configureFailPoint(
                    this.shard0Conn,
                    "suspendRangeDeletion",
                );

                assert.commandWorked(
                    this.testDB.adminCommand({
                        moveChunk: this.collNS,
                        find: {shardKey: 15},
                        to: this.st.shard1.shardName,
                    }),
                );

                // Fetch each shard's current shard version now that the migration has committed, so
                // it can be attached to reproduce the ownership filtering that mongos would normally
                // trigger by routing the request.
                this.shard0Version = ShardVersioningUtil.getShardVersion(
                    this.shard0Conn,
                    this.collNS,
                    true /* waitForRefresh */,
                );
                this.shard1Version = ShardVersioningUtil.getShardVersion(
                    this.shard1Conn,
                    this.collNS,
                    true /* waitForRefresh */,
                );
            });

            after(function () {
                this.suspendRangeDeletionFp.off();
                this.internalConn0.close();
                this.internalConn1.close();
                this.st.stop();
            });

            it("orphaned documents still exist physically on the donor shard", function () {
                assert.eq(this.shard0Coll.find({_id: {$in: [15, 16, 17, 18]}}).itcount(), 4);
            });

            it("filters out orphaned documents left behind by the migration", function () {
                const results = idLookupAgg(
                    this.internalDB0,
                    collName,
                    [
                        {$match: {_id: {$in: [1, 2, 15, 16]}}},
                        {$_internalSearchIdLookup: {}},
                        {$sort: {_id: 1}},
                    ],
                    this.shard0Version,
                ).toArray();

                assert.eq(
                    results.map((doc) => doc._id),
                    [1, 2],
                    "documents migrated away (_id: 15, 16) should be filtered out as orphans",
                );
            });

            it("returns migrated documents as owned on the recipient shard", function () {
                const results = idLookupAgg(
                    this.internalDB1,
                    collName,
                    [
                        {$match: {_id: {$in: [15, 16, 101, 102]}}},
                        {$_internalSearchIdLookup: {}},
                        {$sort: {_id: 1}},
                    ],
                    this.shard1Version,
                ).toArray();

                assert.eq(
                    results.map((doc) => doc._id),
                    [15, 16, 101, 102],
                    "migrated documents should be returned as owned on the recipient",
                );
            });
        },
    );
}
