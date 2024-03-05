/**
 * Verifies that $queryStats operates correctly when upgrading and downgrading.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import "jstests/multiVersion/libs/multi_cluster.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {getQueryStats, verifyMetrics} from "jstests/libs/query_stats_utils.js";
(function() {
"use strict";

const lastLTSVersion = "last-lts";

class UpgradeDowngradeTestFixture {
    constructor() {
        this.collectionName = "coll";
    }

    getTestDB() {
        return this.rst.getPrimary().getDB(jsTestName());
    }

    createAndPopulateTestCollection() {
        let testDB = this.getTestDB();
        let coll = assertDropAndRecreateCollection(testDB, "coll");
        assert.commandWorked(coll.insertMany([{x: 0}, {x: 1}, {x: 2}]));
    }

    setup() {
        jsTestLog("Starting ReplicaSet Upgrade/Downgrade Test!");

        this.rst = new ReplSetTest({
            name: jsTestName(),
            nodes: [{binVersion: lastLTSVersion}, {binVersion: lastLTSVersion}]
        });
        this.rst.startSet();
        this.rst.initiate();
        this.createAndPopulateTestCollection();
    }

    teardown() {
        jsTestLog("Teardown ReplicaSet Upgrade/Downgrade Test!");

        this.rst.stopSet();
    }

    getTestCollection() {
        return this.getTestDB().getCollection(this.collectionName);
    }

    adminCommand(command) {
        return this.rst.getPrimary().getDB("admin").runCommand(command);
    }

    upgradeBinaries() {
        this.rst.upgradeSet({binVersion: "latest"});
    }

    upgradeFCV() {
        assert.commandWorked(
            this.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    }

    downgradeFCV() {
        // Downgrade FCV (without restarting) and check that $queryStats returns an error.
        assert.commandWorked(
            this.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    }

    testQueryStatsParam(expectedResult) {
        if (expectedResult === true) {
            assert.commandWorked(
                this.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1}));
        } else {
            // Check that $queryStats related parameter does not work.
            assert.commandFailedWithCode(
                this.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1}), 7373500);
        }
    }

    testQueryStatsCommandFailure(expectedErrorCode) {
        assert.commandFailedWithCode(
            this.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}),
            expectedErrorCode);
    }

    isShardedCluster() {
        return false;
    }

    verifyQueryStatsOutput({expectedNShapes}) {
        const queryStats = getQueryStats(this);
        assert.eq(expectedNShapes, queryStats.length, queryStats);
        verifyMetrics(queryStats);
    }
}

class ShardedClusterUpgradeDowngradeTestFixture extends UpgradeDowngradeTestFixture {
    setup() {
        jsTestLog("Starting Sharded Upgrade/Downgrade Test!");
        this.shardedTest = new ShardingTest({
            name: jsTestName(),
            shards: 1,
            mongos: 1,
            config: 1,
            rs: {nodes: [{binVersion: lastLTSVersion}, {binVersion: lastLTSVersion}]},
            mongosOptions: {binVersion: lastLTSVersion},
        });
        this.createAndPopulateTestCollection();
    }

    isShardedCluster() {
        return true;
    }

    adminCommand(command) {
        return this.shardedTest.s.adminCommand(command);
    }

    getTestDB() {
        return this.shardedTest.s.getDB(jsTestName());
    }

    upgradeBinaries() {
        this.shardedTest.upgradeCluster('latest');
    }

    teardown() {
        jsTestLog("Teardown ReplicaSet Upgrade/Downgrade Test!");
        this.shardedTest.stop();
    }
}

function runTest(fixture) {
    // 0. Initialize the fixture, we begin with a binary version that doesn't support queryStats.
    fixture.setup();

    // 1. Test that queryStats related features don't work in the base binary.
    // 1.a. Verify queryStats is disabled by checking that setting queryStats related parameters and
    // running the $queryStats stage both fail.
    fixture.testQueryStatsParam(false);
    fixture.testQueryStatsCommandFailure(ErrorCodes.QueryFeatureNotAllowed);

    // 1.b. Run some queries while running base binary. We don't expect these queries to be
    // collected by queryStats.
    {
        let coll = fixture.getTestCollection();
        let res = coll.aggregate([{$match: {x: 2}}]).toArray();
        assert.eq(1, res.length, res);

        res = coll.find({x: 2}).toArray();
        assert.eq(1, res.length, res);
    }

    // 2. Upgrade both the binaries and FCV. After the upgrade we expect query stats features
    // to work. Further, we verify that the previously executed queries were not collected by
    // checking the results from the $queryStats stage.
    fixture.upgradeBinaries();
    fixture.upgradeFCV();

    // 2.a. Verify that queryStats parameters can be set after the upgrade, and that previously
    // executed queries were not collected.
    fixture.testQueryStatsParam(true);
    fixture.verifyQueryStatsOutput({expectedNShapes: 0});

    // 2.b. Execute two new queries (i.e new query shapes) and check that they are collected.
    {
        let coll = fixture.getTestCollection();
        let res = coll.aggregate([{$match: {x: 2}}, {$sort: {x: -1}}]).toArray();
        assert.eq(1, res.length, res);

        res = coll.find({x: 2}, {noCursorTimeout: true}).toArray();
        assert.eq(1, res.length, res);

        fixture.verifyQueryStatsOutput({
            expectedNShapes: 3 /* 1 each for the above, plus $queryStats itself */
        });
    }

    // 3. Downgrade FCV (without restarting). Without restarting the server, we expect the
    // queryStatsStore to keep existing entries, but to not collect any more statistics while an FCV
    // that doesn't support queryStats is in place.
    fixture.downgradeFCV();

    // 3.a. Run additional queries (new query shapes) after the downgrade.
    {
        let coll = fixture.getTestCollection();
        let res = coll.aggregate([{$match: {x: 2}}]).toArray();
        assert.eq(1, res.length, res);

        res = coll.find({x: 2}).toArray();
        assert.eq(1, res.length, res);
    }

    // 3.b. If we are testing a sharded cluster, we don't expect queryStats to be disabled after the
    // FCV downgrade. This is because in a sharded cluster, query stats resides on mongos, and
    // mongos doesn't have a concept of its own in memory FCV settings. When a sharded cluster has
    // its FCV downgraded, it forwards the downgrade request to the shards and the config
    // servers. However, the router itself bases its FCV on its binaries. In our case, once we
    // upgrade the binary on the mongos, we can no longer downgrade the FCV to "disable" queryStats.
    // As a result, in the sharded scenario we expect the two queries run after the downgrade to
    // have been collected by query stats.
    if (fixture.isShardedCluster()) {
        fixture.verifyQueryStatsOutput({
            expectedNShapes: 5 /* 3 from the previous queries + the two additional ones captured
                                  after the downgrade. */
        });
        fixture.teardown();
        return;
    }

    // 3.c. After the FCV downgrade (i.e FCV v7.2), running $queryStats should return an error.
    fixture.testQueryStatsCommandFailure(ErrorCodes.QueryFeatureNotAllowed);

    // 3.d. Upgrade the FCV without restart. QueryStatsStore should not have been cleared. Previous
    // stats collected should be returned. The queries made during the downgrade should not
    // have been collected. Note that getQueryStats() uses the same query to collect the statistics,
    // so running $queryStats doesn't result in additional entries.
    fixture.upgradeFCV();
    fixture.verifyQueryStatsOutput({expectedNShapes: 3});

    // 4. Test query stats during a failed FCV downgrade. During the failed FCV downgrade, the FCV
    // reverts back to the lastLTSFCV which doesn't support queryStats.
    jsTestLog("Turning the failpoint on.");
    assert.commandWorked(
        fixture.adminCommand({configureFailPoint: 'failDowngrading', mode: "alwaysOn"}));

    assert.commandFailedWithCode(
        fixture.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 549181);

    // 4.a. Current FCV is lower than the FCV necessary (7.2) for queryStats to run. These queries
    // should not be collected.
    {
        let coll = fixture.getTestCollection();
        let res = coll.aggregate([{$match: {x: 2}}]).toArray();
        assert.eq(1, res.length, res);

        res = coll.find({x: 2}).toArray();
        assert.eq(1, res.length, res);
    }
    // 4.b. Running $queryStats should return an error.
    fixture.testQueryStatsCommandFailure(ErrorCodes.QueryFeatureNotAllowed);

    // 4.c. Successfully upgrade FCV. Check that the queries run during the failed downgrade were
    // not collected.
    fixture.upgradeFCV();
    fixture.verifyQueryStatsOutput({expectedNShapes: 3});

    // 4.d. Execute two new queries (new query shapes), these should result in new entries.
    {
        let coll = fixture.getTestCollection();
        let res = coll.aggregate([{$match: {x: 2}}], {allowDiskUse: false}).toArray();
        assert.eq(1, res.length, res);

        res = coll.find({x: 2}, {allowPartialResults: true}).toArray();
        assert.eq(1, res.length, res);

        fixture.verifyQueryStatsOutput({expectedNShapes: 5});
    }

    fixture.teardown();
}

// Perform Upgrade/Downgrade test on replica set.
runTest(new UpgradeDowngradeTestFixture());
// Perform Upgrade/Downgrade test on sharded cluster.
runTest(new ShardedClusterUpgradeDowngradeTestFixture());
})();
