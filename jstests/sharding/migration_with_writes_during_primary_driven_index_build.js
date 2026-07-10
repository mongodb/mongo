/**
 * Tests that chunk migration correctly handles retryable writes that occur during a primary-driven
 * index build, where the write is packed into an atomically-applied applyOps entry.
 *
 * @tags: [
 *   requires_replication,
 *   uses_atclustertime,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

describe("chunk migration with writes during primary-driven index build", function () {
    const dbName = "test";
    const collName = jsTestName();
    const ns = dbName + "." + collName;

    beforeEach(function () {
        this.st = new ShardingTest({shards: 2, rs: {nodes: 2}});
        this.mongos = this.st.s;
        this.testDB = this.mongos.getDB(dbName);
        this.testColl = this.testDB.getCollection(collName);
        this.shard0Primary = this.st.rs0.getPrimary();

        if (
            !FeatureFlagUtil.isPresentAndEnabled(
                this.shard0Primary.getDB(dbName),
                "PrimaryDrivenIndexBuilds",
            )
        ) {
            jsTest.log.info(
                "Skipping test because featureFlagPrimaryDrivenIndexBuilds is disabled",
            );
            this.st.stop();
            quit();
        }

        if (
            !FeatureFlagUtil.isPresentAndEnabled(
                this.shard0Primary.getDB(dbName),
                "ContainerWrites",
            )
        ) {
            jsTest.log.info("Skipping test because featureFlagContainerWrites is disabled");
            this.st.stop();
            quit();
        }

        // Set up a sharded collection with all data on shard0.
        assert.commandWorked(
            this.mongos.adminCommand({
                enableSharding: dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        assert.commandWorked(this.mongos.adminCommand({shardCollection: ns, key: {x: 1}}));

        // Insert initial data.
        assert.commandWorked(
            this.testColl.insert([
                {_id: 0, x: -10},
                {_id: 1, x: -5},
                {_id: 2, x: 5},
                {_id: 3, x: 10},
            ]),
        );

        // Split so x < 0 and x >= 0 are separate chunks.
        assert.commandWorked(this.mongos.adminCommand({split: ns, middle: {x: 0}}));
    });

    afterEach(function () {
        this.st.stop();
    });

    it("migrates session state for retryable writes before migration", function () {
        const shard0DB = this.shard0Primary.getDB(dbName);

        // Pause the index build so subsequent writes generate side writes, producing the
        // atomically-applied applyOps format that migration must handle.
        IndexBuildTest.pauseIndexBuilds(this.shard0Primary);
        const awaitIndex = IndexBuildTest.startIndexBuild(this.mongos, ns, {a: 1}, {name: "a_1"});
        IndexBuildTest.waitForIndexBuildToStart(shard0DB, collName, "a_1");

        const lsid = {id: UUID()};
        const insertCommand = {
            insert: collName,
            documents: [{_id: "migrate_test", x: -20}],
            lsid: lsid,
            txnNumber: NumberLong(0),
        };

        jsTest.log.info("Issuing retryable insert during PDIB on shard0");
        assert.commandWorked(this.testDB.runCommand(insertCommand));

        IndexBuildTest.resumeIndexBuilds(this.shard0Primary);
        awaitIndex();

        jsTest.log.info("Migrating chunk from shard0 to shard1");
        assert.commandWorked(
            this.mongos.adminCommand({
                moveChunk: ns,
                find: {x: -20},
                to: this.st.shard1.shardName,
                _waitForDelete: true,
            }),
        );

        const shard1Primary = this.st.rs1.getPrimary();
        const shard1Coll = shard1Primary.getDB(dbName).getCollection(collName);
        assert.eq(
            1,
            shard1Coll.countDocuments({_id: "migrate_test"}),
            "migrated document not found on shard1",
        );

        // Retry through mongos — should be a noop if session state was migrated. The retry
        // response carries retriedStmtIds only when the statement was recognized as already
        // executed, so it is a direct signal that the write did not run again.
        const retryRes = assert.commandWorked(this.testDB.runCommand(insertCommand));
        assert.eq(
            retryRes.retriedStmtIds,
            [NumberInt(0)],
            "retry after migration should be a no-op",
            {res: retryRes},
        );
        assert.eq(
            1,
            this.testColl.countDocuments({_id: "migrate_test"}),
            "retry after migration should not duplicate the document",
        );
    });

    it("migrates session state for retryable writes during migration", function () {
        const shard0DB = this.shard0Primary.getDB(dbName);

        // Pause the index build so subsequent writes generate side writes, producing the
        // atomically-applied applyOps format that migration must handle.
        IndexBuildTest.pauseIndexBuilds(this.shard0Primary);
        const awaitIndex = IndexBuildTest.startIndexBuild(this.mongos, ns, {b: 1}, {name: "b_1"});
        IndexBuildTest.waitForIndexBuildToStart(shard0DB, collName, "b_1");

        // Pause migration before the critical section so we can issue a retryable write mid-migration.
        const hangFp = configureFailPoint(this.shard0Primary, "hangBeforeEnteringCriticalSection");

        // Start migration in the background.
        const migrationThread = new Thread(
            function (mongosHost, ns, shardName) {
                const mongos = new Mongo(mongosHost);
                return mongos.adminCommand({
                    moveChunk: ns,
                    find: {x: -30},
                    to: shardName,
                    _waitForDelete: true,
                });
            },
            this.mongos.host,
            ns,
            this.st.shard1.shardName,
        );

        // We need a document in the x < 0 chunk for migration to transfer.
        assert.commandWorked(
            this.testDB.runCommand({
                insert: collName,
                documents: [{_id: "pre_migrate", x: -30}],
            }),
        );

        migrationThread.start();
        hangFp.wait();

        const duringLsid = {id: UUID()};
        const duringInsertCommand = {
            insert: collName,
            documents: [{_id: "during_migrate", x: -25}],
            lsid: duringLsid,
            txnNumber: NumberLong(0),
        };

        jsTest.log.info("Issuing retryable insert during PDIB while migration is in progress");
        assert.commandWorked(this.testDB.runCommand(duringInsertCommand));

        // Let migration finish.
        hangFp.off();
        migrationThread.join();
        assert.commandWorked(migrationThread.returnData());

        // Resume and complete the index build.
        IndexBuildTest.resumeIndexBuilds(this.shard0Primary);
        awaitIndex();

        // Retry through mongos — should be a noop if session state for the during-migration write
        // was transferred to the recipient. retriedStmtIds in the response confirms the statement
        // was recognized as already executed rather than run again.
        jsTest.log.info("Retrying during-migration insert through mongos");
        const retryRes = assert.commandWorked(this.testDB.runCommand(duringInsertCommand));
        assert.eq(
            retryRes.retriedStmtIds,
            [NumberInt(0)],
            "retry of during-migration write should be a no-op",
            {res: retryRes},
        );
        assert.eq(
            1,
            this.testColl.countDocuments({_id: "during_migrate"}),
            "retry of during-migration write should not duplicate the document",
        );
    });
});
