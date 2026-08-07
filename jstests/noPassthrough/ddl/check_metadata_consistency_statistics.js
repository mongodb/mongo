/*
 * Tests that the checkMetadataConsistency observability metrics reported under
 * serverStatus().shardingStatistics.checkMetadataConsistencyStatistics are updated end-to-end when
 * running the command, and that a database-level check traverses all of its inner collections.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("checkMetadataConsistency observability metrics", function () {
    before(function () {
        // A single shard keeps all databases and collections (and therefore all statistics) on one
        // primary, which makes the counter deltas deterministic.
        this.st = new ShardingTest({shards: 1, name: jsTestName(), other: {enableBalancer: false}});
        this.shardPrimary = this.st.rs0.getPrimary();

        this.getStats = () => {
            const serverStatus = assert.commandWorked(
                this.shardPrimary.adminCommand({serverStatus: 1}),
            );
            assert(serverStatus.shardingStatistics, "missing shardingStatistics section");
            const stats = serverStatus.shardingStatistics.checkMetadataConsistencyStatistics;
            assert(stats, "missing checkMetadataConsistencyStatistics section");
            return stats;
        };

        // Create several collections in the database. Shard some of them so we also exercise the
        // chunk traversal, and leave one unsharded so the collection count includes it too.
        this.dbName = jsTestName();
        this.db = this.st.s.getDB(this.dbName);
        this.shardedColls = ["sharded0", "sharded1", "sharded2"];
        for (const collName of this.shardedColls) {
            assert.commandWorked(
                this.st.s.adminCommand({
                    shardCollection: `${this.dbName}.${collName}`,
                    key: {_id: 1},
                }),
            );
        }
        assert.commandWorked(this.db.createCollection("unsharded0"));

        // The number of collections the database-level traversal will visit.
        this.collInfos = this.db.getCollectionInfos();
        this.numCollectionsInDb = this.collInfos.length;
        assert.gte(
            this.numCollectionsInDb,
            this.shardedColls.length + 1,
            "unexpected collections",
            {
                collInfos: this.collInfos,
            },
        );
    });

    after(function () {
        this.st.stop();
    });

    it("exposes every expected metric and reports no active locks when idle", function () {
        const stats = this.getStats();
        for (const field of [
            "numberOfInconsistenciesFound",
            "numberOfDatabasesChecked",
            "numberOfCollectionsChecked",
            "numberOfChunksChecked",
            "activeDdlLocksHeldForDatabase",
            "activeDdlLocksHeldForDatabaseDurationMillis",
            "activeDdlLocksHeldForCollection",
            "activeDdlLocksHeldForCollectionDurationMillis",
            "ddlLockHeldForDatabaseDurationMillis",
            "ddlLockHeldForCollectionDurationMillis",
        ]) {
            assert(stats.hasOwnProperty(field), `missing metric '${field}'`, {stats});
        }

        // No check is running, so there should be no active DDL locks held.
        assert.eq(0, stats.activeDdlLocksHeldForDatabase, "unexpected active database locks", {
            stats,
        });
        assert.eq(0, stats.activeDdlLocksHeldForCollection, "unexpected active collection locks", {
            stats,
        });
    });

    it("counts one database and all of its inner collections on a database-level check", function () {
        const before = this.getStats();
        assert.commandWorked(this.db.runCommand({checkMetadataConsistency: 1}));
        const after = this.getStats();

        jsTest.log.info("Database-level check statistics", {before, after});

        // Exactly one database was traversed.
        assert.eq(
            1,
            after.numberOfDatabasesChecked - before.numberOfDatabasesChecked,
            "database-level check should count exactly one database",
        );

        // Traversing the database must visit every collection it contains.
        assert.eq(
            this.numCollectionsInDb,
            after.numberOfCollectionsChecked - before.numberOfCollectionsChecked,
            "database check should count all inner collections",
        );

        // Sharded collections have their chunks traversed, so the chunk counter must advance.
        assert.gte(
            after.numberOfChunksChecked - before.numberOfChunksChecked,
            this.shardedColls.length,
            "chunk traversal should count at least one chunk per sharded collection",
        );

        // After the command completes no DDL locks should remain active, and any time the locks were
        // held during the check must have been added to the cumulative totals.
        assert.eq(0, after.activeDdlLocksHeldForDatabase, "database locks still active", {after});
        assert.eq(0, after.activeDdlLocksHeldForCollection, "collection locks still active", {
            after,
        });
        assert.gte(
            after.ddlLockHeldForDatabaseDurationMillis,
            before.ddlLockHeldForDatabaseDurationMillis,
            "cumulative database DDL lock duration must not decrease",
        );
        assert.gte(
            after.ddlLockHeldForCollectionDurationMillis,
            before.ddlLockHeldForCollectionDurationMillis,
            "cumulative collection DDL lock duration must not decrease",
        );
    });

    it("counts only the targeted collection on a collection-level check", function () {
        const before = this.getStats();
        assert.commandWorked(this.db.runCommand({checkMetadataConsistency: this.shardedColls[0]}));
        const after = this.getStats();

        jsTest.log.info("Collection-level check statistics", {before, after});

        // A collection-level check does not traverse a database, so the database counter must not
        // advance.
        assert.eq(
            0,
            after.numberOfDatabasesChecked - before.numberOfDatabasesChecked,
            "collection-level check should not count any database",
        );

        // Exactly one collection, the targeted one, is checked.
        assert.eq(
            1,
            after.numberOfCollectionsChecked - before.numberOfCollectionsChecked,
            "collection-level check should count exactly the targeted collection",
        );

        // The targeted collection is sharded, so its chunks must be traversed.
        assert.gte(
            after.numberOfChunksChecked - before.numberOfChunksChecked,
            1,
            "chunk traversal should count the targeted collection's chunks",
        );

        // No DDL locks should remain active once the check completes.
        assert.eq(0, after.activeDdlLocksHeldForDatabase, "database locks still active", {after});
        assert.eq(0, after.activeDdlLocksHeldForCollection, "collection locks still active", {
            after,
        });
    });

    it("traverses the collections of every database on a cluster-level check", function () {
        const before = this.getStats();
        assert.commandWorked(this.st.s.adminCommand({checkMetadataConsistency: 1}));
        const after = this.getStats();

        jsTest.log.info("Cluster-level check statistics", {before, after});

        // A cluster-level check traverses every user database, including the one created in setup, so
        // the collection counter must advance by at least the collections in our database.
        assert.gte(
            after.numberOfCollectionsChecked - before.numberOfCollectionsChecked,
            this.numCollectionsInDb,
            "cluster check should traverse the collections of every database",
        );
    });
});
