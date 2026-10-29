/**
 * Tests that the 'filter' option of the listDatabases command is applied to the aggregated reply
 * on mongos, not merely forwarded to each shard.
 *
 * The aggregated reply contains fields ('sizeOnDisk', 'empty', 'shards') whose values are computed
 * on mongos and can differ from what any individual shard reports:
 *   - 'sizeOnDisk' is the sum of the per-shard sizes, so it can exceed every individual shard's
 *     value.
 *   - 'shards' only exists in the aggregated mongos reply; individual shard replies do not have it.
 *   - 'empty' is recomputed on mongos from the aggregated size.
 * Evaluating the filter only on the shards therefore either drops databases that should match or
 * keeps ones that should not. This test exercises those cases against a two-shard cluster and also
 * verifies that name-based and nameOnly filtering still behave correctly.
 *
 * It also covers databases that are known to the config server (via 'enableSharding') but do not
 * yet have any collections on any shard. mongos seeds these as empty entries from the config
 * server snapshot; that seeding must happen (and the resulting entries must be filterable)
 * regardless of which fields the filter references, not just for filters on 'name'.
 *
 * @tags: [
 *   # The mongos-side filter re-application is not present on older mongos binaries.
 *   multiversion_incompatible,
 *   # The test reasons about deterministic chunk placement across shards.
 *   assumes_balancer_off,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("listDatabases filter on mongos", function () {
    const dbOnShard0 = jsTestName() + "_s0";
    const dbOnShard1 = jsTestName() + "_s1";
    const dbOnBoth = jsTestName() + "_both";
    // Known to the config server (via 'enableSharding') but never given any collections, so it
    // never appears in any shard's own listDatabases reply. mongos must seed it from the config
    // server snapshot.
    const dbEmpty = jsTestName() + "_empty";
    const namePrefix = new RegExp("^" + jsTestName() + "_");

    before(function () {
        this.st = new ShardingTest({shards: 2, mongos: 1});
        this.mongos = this.st.s0;
        this.shard0Name = this.st.shard0.shardName;
        this.shard1Name = this.st.shard1.shardName;

        const mongos = this.mongos;

        // Place the databases on deterministic primary shards so we can reason about per-shard vs
        // aggregated values.
        assert.commandWorked(
            mongos.adminCommand({enableSharding: dbOnShard0, primaryShard: this.shard0Name}),
        );
        assert.commandWorked(
            mongos.adminCommand({enableSharding: dbOnShard1, primaryShard: this.shard1Name}),
        );
        assert.commandWorked(
            mongos.adminCommand({enableSharding: dbOnBoth, primaryShard: this.shard0Name}),
        );
        // Registered with the config server but deliberately given no collections.
        assert.commandWorked(
            mongos.adminCommand({enableSharding: dbEmpty, primaryShard: this.shard0Name}),
        );

        // Give each database a non-zero size on its primary shard via an unsharded collection.
        assert.commandWorked(mongos.getDB(dbOnShard0).coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));
        assert.commandWorked(mongos.getDB(dbOnShard1).coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));
        assert.commandWorked(mongos.getDB(dbOnBoth).coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));

        // Spread 'dbOnBoth' across both shards with a sharded collection that has a chunk (and
        // data) on each shard, so its aggregated size strictly exceeds either per-shard size.
        const shardedNs = `${dbOnBoth}.shardedColl`;
        assert.commandWorked(mongos.getDB(dbOnBoth).shardedColl.createIndex({x: 1}));
        assert.commandWorked(mongos.adminCommand({shardCollection: shardedNs, key: {x: 1}}));
        assert.commandWorked(mongos.adminCommand({split: shardedNs, middle: {x: 0}}));
        assert.commandWorked(
            mongos.adminCommand({moveChunk: shardedNs, find: {x: 0}, to: this.shard1Name}),
        );
        assert.commandWorked(
            mongos
                .getDB(dbOnBoth)
                .shardedColl.insertMany([{x: -3}, {x: -2}, {x: -1}, {x: 1}, {x: 2}, {x: 3}]),
        );
    });

    after(function () {
        this.st.stop();
    });

    // Run listDatabases through mongos and return the full reply.
    function listDbs(mongos, options) {
        return assert.commandWorked(
            mongos.adminCommand(Object.assign({listDatabases: 1}, options)),
        );
    }

    function findDb(res, name) {
        return res.databases.find((d) => d.name === name) || null;
    }

    function getDbNames(res) {
        return res.databases.map((d) => d.name).sort();
    }

    it("returns the expected test databases without a filter", function () {
        const res = listDbs(this.mongos, {});
        const names = getDbNames({databases: res.databases.filter((d) => namePrefix.test(d.name))});
        assert.eq([dbEmpty, dbOnBoth, dbOnShard0, dbOnShard1].sort(), names, tojson(res));

        const emptyEntry = findDb(res, dbEmpty);
        assert.neq(null, emptyEntry, tojson(res));
        assert.eq(0, emptyEntry.sizeOnDisk, tojson(emptyEntry));
        assert.eq(true, emptyEntry.empty, tojson(emptyEntry));
        // No shard ever reported this database, so mongos never synthesises a 'shards' field for
        // it, unlike the populated databases.
        assert(!emptyEntry.hasOwnProperty("shards"), tojson(emptyEntry));
    });

    it("applies a name filter identically to unfiltered plus client-side filtering", function () {
        const filtered = listDbs(this.mongos, {filter: {name: dbOnBoth}});
        assert.eq(1, filtered.databases.length, tojson(filtered));
        assert.eq(dbOnBoth, filtered.databases[0].name, tojson(filtered));

        const unfiltered = findDb(listDbs(this.mongos, {}), dbOnBoth);
        assert.eq(unfiltered.sizeOnDisk, filtered.databases[0].sizeOnDisk, tojson(filtered));
    });

    it("applies a 'shards' filter against the mongos-only aggregated field", function () {
        // The 'shards' subdocument does not exist on individual shard replies, so a shard-side
        // evaluation of this predicate would drop every database. It must be evaluated on mongos.
        const onShard0 = listDbs(this.mongos, {
            filter: {name: namePrefix, [`shards.${this.shard0Name}`]: {$exists: true}},
        });
        const names0 = getDbNames(onShard0);
        // 'dbOnShard0' (primary shard0) and 'dbOnBoth' (primary shard0, chunk on shard1) have data
        // on shard0; 'dbOnShard1' does not.
        assert.eq([dbOnBoth, dbOnShard0].sort(), names0, tojson(onShard0));

        const onShard1 = listDbs(this.mongos, {
            filter: {name: namePrefix, [`shards.${this.shard1Name}`]: {$exists: true}},
        });
        const names1 = getDbNames(onShard1);
        // 'dbOnShard1' (primary shard1) and 'dbOnBoth' (chunk moved to shard1) have data on shard1.
        assert.eq([dbOnBoth, dbOnShard1].sort(), names1, tojson(onShard1));
    });

    it("applies a 'sizeOnDisk' filter against the aggregated total", function () {
        // Read the per-shard sizes from the aggregated 'shards' subdocument. The aggregated
        // sizeOnDisk is their sum, which must strictly exceed the largest single-shard size when a
        // database has data on more than one shard.
        const bothEntry = findDb(listDbs(this.mongos, {}), dbOnBoth);
        assert.neq(null, bothEntry, "expected dbOnBoth in listDatabases");
        assert(bothEntry.shards, tojson(bothEntry));

        const perShardSizes = Object.values(bothEntry.shards);
        assert.gte(perShardSizes.length, 2, tojson(bothEntry));
        const maxPerShard = Math.max(...perShardSizes);
        const aggregated = bothEntry.sizeOnDisk;
        assert.gt(aggregated, maxPerShard, tojson(bothEntry));

        // A threshold between the largest per-shard size and the aggregated total: each shard fails
        // the predicate individually, but the aggregated database passes. A shard-side evaluation
        // would incorrectly drop the database.
        const passing = listDbs(this.mongos, {
            filter: {name: dbOnBoth, sizeOnDisk: {$gt: maxPerShard}},
        });
        assert.eq(1, passing.databases.length, tojson(passing));
        assert.eq(dbOnBoth, passing.databases[0].name, tojson(passing));
        assert.gt(passing.databases[0].sizeOnDisk, maxPerShard, tojson(passing));

        // A threshold above the aggregated total must exclude the database, proving the filter is
        // actually applied.
        const excluded = listDbs(this.mongos, {
            filter: {name: dbOnBoth, sizeOnDisk: {$gt: aggregated}},
        });
        assert.eq(0, excluded.databases.length, tojson(excluded));
    });

    it("applies an 'empty' filter against the aggregated size", function () {
        const filtered = listDbs(this.mongos, {filter: {name: namePrefix, empty: false}});
        assert.eq(
            [dbOnBoth, dbOnShard0, dbOnShard1].sort(),
            getDbNames(filtered),
            tojson(filtered),
        );
        for (const d of filtered.databases) {
            assert.eq(false, d.empty, tojson(d));
        }

        // Only 'dbEmpty' among the test databases is empty.
        const empties = listDbs(this.mongos, {filter: {name: namePrefix, empty: true}});
        assert.eq([dbEmpty], getDbNames(empties), tojson(empties));
    });

    it("seeds and filters config-server-only empty databases regardless of the filter's fields", function () {
        // A filter combining 'name' with an aggregated-only field like 'empty' must still
        // surface a database that is known only via the config server snapshot.
        const byNameAndEmpty = listDbs(this.mongos, {filter: {name: dbEmpty, empty: true}});
        assert.eq([dbEmpty], getDbNames(byNameAndEmpty), tojson(byNameAndEmpty));

        const byNameAndSize = listDbs(this.mongos, {filter: {name: dbEmpty, sizeOnDisk: 0}});
        assert.eq([dbEmpty], getDbNames(byNameAndSize), tojson(byNameAndSize));

        // 'shards' is never set on a config-server-only entry, so a predicate requiring its
        // absence must match it.
        const byMissingShards = listDbs(this.mongos, {
            filter: {name: dbEmpty, shards: {$exists: false}},
        });
        assert.eq([dbEmpty], getDbNames(byMissingShards), tojson(byMissingShards));

        // A mismatched non-name predicate must still correctly exclude it.
        const excluded = listDbs(this.mongos, {filter: {name: dbEmpty, empty: false}});
        assert.eq(0, excluded.databases.length, tojson(excluded));
    });

    it("combines predicates across multiple aggregated fields", function () {
        const filtered = listDbs(this.mongos, {
            filter: {
                name: namePrefix,
                empty: false,
                [`shards.${this.shard1Name}`]: {$exists: true},
            },
        });
        const names = getDbNames(filtered);
        // Only databases with data on shard1: 'dbOnShard1' and 'dbOnBoth'.
        assert.eq([dbOnBoth, dbOnShard1].sort(), names, tojson(filtered));
    });

    it("respects the filter on the nameOnly code path", function () {
        const filtered = listDbs(this.mongos, {nameOnly: true, filter: {name: namePrefix}});
        assert.eq(
            [dbEmpty, dbOnBoth, dbOnShard0, dbOnShard1].sort(),
            getDbNames(filtered),
            tojson(filtered),
        );
        for (const d of filtered.databases) {
            assert.eq(["name"], Object.keys(d), tojson(d));
        }
    });

    it("returns an empty result with totalSize 0 when nothing matches", function () {
        const filtered = listDbs(this.mongos, {filter: {name: jsTestName() + "_does_not_exist"}});
        assert.eq(0, filtered.databases.length, tojson(filtered));
        assert.eq(0, filtered.totalSize, tojson(filtered));
    });

    it("still rejects unsupported filter expressions", function () {
        assert.commandFailed(
            this.mongos.adminCommand({listDatabases: 1, filter: {$text: {$search: "str"}}}),
        );
        assert.commandFailed(
            this.mongos.adminCommand({
                listDatabases: 1,
                filter: {
                    $where: function () {
                        return true;
                    },
                },
            }),
        );
    });
});
