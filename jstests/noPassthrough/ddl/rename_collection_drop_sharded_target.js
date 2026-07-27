/**
 * Tests that renameCollection with dropTarget=true removes a sharded target collection from
 * non-data-bearing participant shards. The source collection lives on a single shard (untracked
 * or unsplittable) while the existing target is sharded across both shards.
 *
 * @tags: [
 *   requires_replication,
 *   requires_2_or_more_shards,
 *   assumes_balancer_off,
 *   uses_rename,
 * ]
 */
import {
    createCollectionAndInsertDocuments,
    setupTestDatabase,
    verifyCollectionTrackingState,
} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

describe("renameCollection drops sharded target on non-data-bearing shards", function () {
    const kWithinDbName = `${jsTestName()}_db`;
    const kSourceDbName = `${jsTestName()}_source`;
    const kTargetDbName = `${jsTestName()}_target`;
    const kSourceCollName = "source";
    const kTargetCollName = "target";
    const kSourceDocuments = [
        {_id: 0, value: "source-0"},
        {_id: 1, value: "source-1"},
    ];
    // x: 0 and x: 2 are placed on different shards via split + moveChunk in setupShardedTargetOnBothShards.
    const kOldTargetDocuments = [
        {x: 0, value: "old-target-0"},
        {x: 2, value: "old-target-2"},
    ];

    before(function () {
        this.shardingTest = new ShardingTest({shards: 2, rs: {nodes: 1}});
    });

    afterEach(function () {
        assert.commandWorked(this.shardingTest.s.getDB(kWithinDbName).dropDatabase());
        assert.commandWorked(this.shardingTest.s.getDB(kSourceDbName).dropDatabase());
        assert.commandWorked(this.shardingTest.s.getDB(kTargetDbName).dropDatabase());
    });

    after(function () {
        this.shardingTest.stop();
    });

    function setupShardedTargetOnBothShards(shardingTest, testDb, collName) {
        const dbName = testDb.getName();
        const nss = `${dbName}.${collName}`;

        assert.commandWorked(
            shardingTest.s.adminCommand({
                enableSharding: dbName,
                primaryShard: shardingTest.shard0.shardName,
            }),
        );
        assert.commandWorked(shardingTest.s.adminCommand({shardCollection: nss, key: {x: 1}}));
        assert.commandWorked(testDb[collName].insert(kOldTargetDocuments));
        assert.commandWorked(shardingTest.s.adminCommand({split: nss, middle: {x: 1}}));
        assert.commandWorked(
            shardingTest.s.adminCommand({
                moveChunk: nss,
                find: {x: 2},
                to: shardingTest.shard1.shardName,
                _waitForDelete: true,
            }),
        );

        const primaryColl = shardingTest.shard0.rs.getPrimary().getDB(dbName)[collName];
        const nonPrimaryColl = shardingTest.shard1.rs.getPrimary().getDB(dbName)[collName];
        assert.eq(1, primaryColl.find({x: 0}).itcount(), "expected x:0 on the primary shard");
        assert.eq(0, primaryColl.find({x: 2}).itcount(), "expected no x:2 on the primary shard");
        assert.eq(
            1,
            nonPrimaryColl.find({x: 2}).itcount(),
            "expected x:2 on the non-primary shard",
        );
        assert.eq(
            0,
            nonPrimaryColl.find({x: 0}).itcount(),
            "expected no x:0 on the non-primary shard",
        );
    }

    function setupTestCaseForWithinDb(shardingTest, collectionType) {
        const sourceDataShard =
            collectionType === "unsplittable" ? shardingTest.shard1 : shardingTest.shard0;
        const nonDataShard =
            collectionType === "unsplittable" ? shardingTest.shard0 : shardingTest.shard1;
        const testDb = setupTestDatabase(
            shardingTest.s.getDB("admin"),
            kWithinDbName,
            shardingTest.shard0.shardName,
        );
        createCollectionAndInsertDocuments(
            collectionType,
            testDb,
            kSourceCollName,
            kSourceDocuments,
            sourceDataShard,
        );
        setupShardedTargetOnBothShards(shardingTest, testDb, kTargetCollName);

        return {
            commandDb: testDb,
            sourceDb: testDb,
            targetDb: testDb,
            sourceNss: `${kWithinDbName}.${kSourceCollName}`,
            targetNss: `${kWithinDbName}.${kTargetCollName}`,
            sourceDataShard,
            nonDataShard,
        };
    }

    function setupTestCaseForCrossDb(shardingTest, collectionType) {
        const sourceDataShard =
            collectionType === "unsplittable" ? shardingTest.shard1 : shardingTest.shard0;
        const nonDataShard =
            collectionType === "unsplittable" ? shardingTest.shard0 : shardingTest.shard1;
        const sourceDb = setupTestDatabase(
            shardingTest.s.getDB("admin"),
            kSourceDbName,
            shardingTest.shard0.shardName,
        );
        const targetDb = setupTestDatabase(
            shardingTest.s.getDB("admin"),
            kTargetDbName,
            shardingTest.shard0.shardName,
        );
        createCollectionAndInsertDocuments(
            collectionType,
            sourceDb,
            kSourceCollName,
            kSourceDocuments,
            sourceDataShard,
        );
        setupShardedTargetOnBothShards(shardingTest, targetDb, kTargetCollName);

        return {
            commandDb: sourceDb,
            sourceDb,
            targetDb,
            sourceNss: `${kSourceDbName}.${kSourceCollName}`,
            targetNss: `${kTargetDbName}.${kTargetCollName}`,
            sourceDataShard,
            nonDataShard,
            oldTargetUUID: targetDb[kTargetCollName].getUUID(),
        };
    }

    function verifyLocalDocumentCountInShard(shard, dbName, collName, expectedCount) {
        // Read directly from the shard's primary so we observe its physical local contents
        // (no routing, no orphan filtering) rather than the cluster-wide view.
        const localDocuments = shard.rs.getPrimary().getDB(dbName)[collName].find().toArray();
        assert.eq(
            expectedCount,
            localDocuments.length,
            `unexpected local target document count on shard ${shard.shardName}`,
            {dbName, collName, localDocuments},
        );
    }

    function assertPerShardTargetCleanup(testCase) {
        const targetDbName = testCase.targetDb.getName();
        // The data-bearing shard holds the renamed collection's documents.
        verifyLocalDocumentCountInShard(
            testCase.sourceDataShard,
            targetDbName,
            kTargetCollName,
            kSourceDocuments.length,
        );
        // The non-data-bearing shard may retain an empty local placeholder (e.g. the DB-primary
        // shard for an unsplittable collection), but it must hold none of the pre-existing
        // sharded target's data.
        verifyLocalDocumentCountInShard(testCase.nonDataShard, targetDbName, kTargetCollName, 0);
    }

    function assertRenameSuccessful(shardingTest, collectionType, testCase, scope) {
        assert(!testCase.sourceDb[kSourceCollName].exists(), "source collection still exists", {
            namespace: testCase.sourceNss,
        });
        assertArrayEq({
            actual: testCase.targetDb[kTargetCollName].find().toArray(),
            expected: kSourceDocuments,
        });
        assertPerShardTargetCleanup(testCase);

        if (scope === "cross-db") {
            const newTargetUUID = testCase.targetDb[kTargetCollName].getUUID();
            assert.neq(
                testCase.oldTargetUUID,
                newTargetUUID,
                "pre-existing target collection was not replaced by the rename",
                {namespace: testCase.targetNss},
            );
        }

        if (collectionType === "unsplittable") {
            verifyCollectionTrackingState(shardingTest.s.getDB("admin"), testCase.sourceNss, false);
            verifyCollectionTrackingState(
                shardingTest.s.getDB("admin"),
                testCase.targetNss,
                true,
                true,
            );
        }
    }

    function runRenameWithShardedTarget(shardingTest, {scope, collectionType}) {
        const testCase =
            scope === "within-db"
                ? setupTestCaseForWithinDb(shardingTest, collectionType)
                : setupTestCaseForCrossDb(shardingTest, collectionType);

        assert.commandWorked(
            testCase.commandDb.adminCommand({
                renameCollection: testCase.sourceNss,
                to: testCase.targetNss,
                dropTarget: true,
            }),
        );

        assertRenameSuccessful(shardingTest, collectionType, testCase, scope);
    }

    it("within-db rename of untracked collection drops sharded target on non-data-bearing shard", function testWithinDbUntracked() {
        runRenameWithShardedTarget(this.shardingTest, {
            scope: "within-db",
            collectionType: "untracked",
        });
    });

    it("within-db rename of unsplittable collection drops sharded target on non-data-bearing shard", function testWithinDbUnsplittable() {
        runRenameWithShardedTarget(this.shardingTest, {
            scope: "within-db",
            collectionType: "unsplittable",
        });
    });

    it("cross-db rename of untracked collection drops sharded target on non-data-bearing shard", function testCrossDbUntracked() {
        runRenameWithShardedTarget(this.shardingTest, {
            scope: "cross-db",
            collectionType: "untracked",
        });
    });

    it("cross-db rename of unsplittable collection drops sharded target on non-data-bearing shard", function testCrossDbUnsplittable() {
        runRenameWithShardedTarget(this.shardingTest, {
            scope: "cross-db",
            collectionType: "unsplittable",
        });
    });
});
