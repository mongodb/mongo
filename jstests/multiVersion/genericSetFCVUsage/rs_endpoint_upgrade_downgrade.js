/*
 * Tests that as long as the replica set endpoint enabled, the connection to a standalone or replica
 * set works across upgrade and downgrade.
 *
 * @tags: [
 *    requires_fcv_80,
 *    featureFlagReplicaSetEndpoint,
 *    featureFlagRouterPort,
 *    # TODO SERVER-98511: Enable this test.
 *    __TEMPORARILY_DISABLED__,
 * ]
 */

import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    extractReplicaSetNameAndHosts,
    makeReplicaSetConnectionString,
    makeStandaloneConnectionString,
    waitForAutoBootstrap
} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

function reconnect(conn) {
    try {
        assert.commandWorked(conn.adminCommand({hello: 1}));
    } catch (e) {
        // For replica set connection strings, the shell has internal auto-reconnect logic
        // so the first command doesn't always fail.
        assert(isNetworkError(e), e);
    }
}

function runTest(connString, getShard0PrimaryFunc, upgradeFunc, downgradeFunc, tearDownFunc) {
    jsTest.log("Running tests for connection string: " + connString);

    const dbName = "testDb";
    const collName = "testColl";

    let conn = new Mongo(connString);
    conn.getDB(dbName).getCollection(collName).insert([{x: 1}]);

    jsTest.log("Start upgrading");
    upgradeFunc();
    jsTest.log("Finished upgrading");
    let shard0Primary = getShard0PrimaryFunc();
    assert.commandWorked(
        shard0Primary.adminCommand({transitionToShardedCluster: 1, writeConcern: {w: "majority"}}));
    waitForAutoBootstrap(shard0Primary);

    // Reconnect after the connection was closed due to restart.
    reconnect(conn);

    const docAfterUpgrade = conn.getDB(dbName).getCollection(collName).findOne({x: 1});
    assert.neq(docAfterUpgrade, null);

    jsTest.log("Start downgrading");
    downgradeFunc();
    jsTest.log("Finished downgrading");

    // Reconnect after the connection was closed due to restart.
    reconnect(conn);
    const docAfterDowngrade = conn.getDB(dbName).getCollection(collName).findOne({x: 1});
    assert.neq(docAfterDowngrade, null);

    tearDownFunc();
}

function runStandaloneTest(oldBinVersion, oldFCVVersion) {
    let node = MongoRunner.runMongod({binVersion: oldBinVersion});

    const connString = makeStandaloneConnectionString(node.host, "admin" /* defaultDbName */);
    const getShard0PrimaryFunc = () => {
        return node;
    };
    const upgradeFunc = () => {
        MongoRunner.stopMongod(node, null, {noCleanData: true});
        node = MongoRunner.runMongod({
            noCleanData: true,
            port: node.port,
            binVersion: "latest",
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            }
        });
        assert.soon(() => {
            const res = assert.commandWorked(node.adminCommand({hello: 1}));
            return res.isWritablePrimary;
        });
        assert.commandWorked(
            node.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };
    const downgradeFunc = () => {
        assert.commandWorked(
            node.adminCommand({setFeatureCompatibilityVersion: oldFCVVersion, confirm: true}));
        MongoRunner.stopMongod(node, null, {noCleanData: true});
        node =
            MongoRunner.runMongod({noCleanData: true, port: node.port, binVersion: oldBinVersion});
    };
    const tearDownFunc = () => MongoRunner.stopMongod(node);
    runTest(connString, getShard0PrimaryFunc, upgradeFunc, downgradeFunc, tearDownFunc);
}

function runReplicaSetTest(oldBinVersion, oldFCVVersion) {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: oldBinVersion}});
    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    const connStringOpts = extractReplicaSetNameAndHosts(rst.getPrimary());
    const connString = makeReplicaSetConnectionString(
        connStringOpts.rsName, connStringOpts.rsHosts, "admin" /* defaultDbName */);
    const getShard0PrimaryFunc = () => {
        return rst.getPrimary();
    };
    const upgradeFunc = () => {
        rst.upgradeSet({
            binVersion: "latest",
            setParameter: {
                featureFlagAllMongodsAreSharded: true,
            }
        });
        assert.commandWorked(rst.getPrimary().adminCommand(
            {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };
    const downgradeFunc = () => {
        assert.commandWorked(rst.getPrimary().adminCommand(
            {setFeatureCompatibilityVersion: oldFCVVersion, confirm: true}));
        rst.upgradeSet({binVersion: oldBinVersion, setParameter: {}});
    };
    const tearDownFunc = () => rst.stopSet();
    runTest(connString, getShard0PrimaryFunc, upgradeFunc, downgradeFunc, tearDownFunc);
}

jsTest.log("Running tests for a 'last-lts' standalone bootstrapped as a single-shard cluster");
runStandaloneTest("last-lts", lastLTSFCV);

jsTest.log(
    "Running tests for a 'last-continuous' standalone bootstrapped as a single-shard cluster");
runStandaloneTest("last-continuous", lastContinuousFCV);

jsTest.log("Running tests for a 'last-lts' replica set bootstrapped as a single-shard cluster");
runReplicaSetTest("last-lts", lastLTSFCV);

jsTest.log(
    "Running tests for a 'last-continuous' replica set bootstrapped as a single-shard cluster");
runReplicaSetTest("last-continuous", lastContinuousFCV);
