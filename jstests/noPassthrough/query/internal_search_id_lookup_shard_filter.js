/**
 * Tests that $_internalSearchIdLookup has shard filtering applied to it so that orphaned
 * documents are not returned.
 *
 * `$_internalSearchIdLookup` resolves `_id`s through one of two executors depending on the
 * `featureFlagSearchOptimizedIdLookup` IFR flag: the Express fast path (flag on) or the classic
 * local-read path (flag off). Because the flag is toggleable at runtime, we run the suite once per
 * state so orphan filtering is exercised on both executors.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

const dbName = "test";
const collName = "internal_search_id_lookup_shard_filter";

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

// TODO SERVER-130493 Remove optimizedIdLookup when no-ff search variant exists.
for (const optimizedIdLookup of [false, true]) {
    describe(
        `$_internalSearchIdLookup shard filtering ` +
            `(featureFlagSearchOptimizedIdLookup=${optimizedIdLookup})`,
        function () {
            before(function () {
                // This test deliberately creates orphans to test shard filtering.
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

                assert.commandWorked(
                    this.testDB.adminCommand({
                        enableSharding: dbName,
                        primaryShard: this.st.shard0.name,
                    }),
                );

                // Documents that end up on shard0.
                for (const id of [1, 2, 3, 4]) {
                    assert.commandWorked(this.testColl.insert({_id: id, shardKey: 0}));
                }
                // Documents that end up on shard1.
                for (const id of [11, 12, 13, 14]) {
                    assert.commandWorked(this.testColl.insert({_id: id, shardKey: 100}));
                }

                assert.commandWorked(this.testColl.createIndex({shardKey: 1}));
                // 'waitForDelete' is true so range deletion completes before the orphan is
                // inserted.
                this.st.shardColl(
                    this.testColl,
                    {shardKey: 1},
                    {shardKey: 10},
                    {shardKey: 10 + 1},
                    dbName,
                    true /* waitForDelete */,
                );

                this.shard0Conn = this.st.rs0.getPrimary();
                this.shard0Coll = this.shard0Conn.getDB(dbName)[collName];
                this.collNS = this.testColl.getFullName();

                // Insert an orphan directly on shard0: a document whose shard key belongs to
                // shard1's owned range.
                assert.commandWorked(this.shard0Coll.insert({_id: 15, shardKey: 100}));

                // Fetch shard0's current shard version for the collection. A bare command sent
                // directly to a shard with no shardVersion attached skips ownership filtering
                // entirely (OperationShardingState never gets populated), so we attach it by hand
                // to reproduce the filtering that mongos would normally trigger by routing the
                // request.
                this.shardVersion = ShardVersioningUtil.getShardVersion(
                    this.shard0Conn,
                    this.collNS,
                );

                // Create an internal client connection directly to shard0 to exercise the
                // internal-only stage.
                this.internalConn = new Mongo(this.shard0Conn.host);
                assert.commandWorked(
                    this.internalConn.getDB("admin").runCommand({
                        hello: 1,
                        internalClient: {
                            minWireVersion: NumberInt(0),
                            maxWireVersion: NumberInt(7),
                        },
                    }),
                );
                this.internalDB = this.internalConn.getDB(dbName);
            });

            after(function () {
                this.internalConn.close();
                this.st.stop();
            });

            it("orphan document exists on shard0 when queried directly", function () {
                assert.eq(this.shard0Coll.find({_id: 15}).itcount(), 1);
            });

            it("filters out the orphan when looked up via $_internalSearchIdLookup", function () {
                // Only _id 1, 2, and 15 physically exist on shard0 (11 was routed to shard1 by
                // mongos); 15 is the orphan and should be dropped by shard filtering.
                const results = idLookupAgg(
                    this.internalDB,
                    collName,
                    [
                        {$match: {_id: {$in: [1, 2, 15]}}},
                        {$_internalSearchIdLookup: {}},
                        {$sort: {_id: 1}},
                    ],
                    this.shardVersion,
                ).toArray();

                assert.eq(
                    results.map((doc) => doc._id),
                    [1, 2],
                    "orphaned document (_id: 15) should be filtered out",
                );
            });
        },
    );
}
