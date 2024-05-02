/*
 * Tests valid/invalid rename operations over timeseries collections
 *
 * @tags: [
 *  uses_rename,
 *  requires_timeseries,
 *  # the rename command is not idempotent
 *  requires_non_retryable_commands,
 *  # rename only works across databases with same primary shard
 *  # TODO SERVER-90096: change this tag with a more specific one
 *  assumes_balancer_off,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const dbName = db.getName();
const otherDbName = `${dbName}_other`;
const collName = "coll";
const bucketsCollName = `system.buckets.${collName}`;
const timeseriesOpts = {
    timeseries: {timeField: "time"}
};

function currentFCV() {
    const node = FixtureHelpers.getPrimaryForNodeHostingDatabase(db);
    const fcvDoc = node.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
    jsTest.log(`${tojson(fcvDoc)}`);
    return fcvDoc.featureCompatibilityVersion.version;
}

function setupEnv() {
    db.dropDatabase();
    db.getSiblingDB(otherDbName).dropDatabase();

    // TODO SERVER-89086 remove the following workaround once the underlying problem is fixed.
    if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
        // On sharded cluster rename throw NamespaceNotFound if the target database does not exists
        // yet. Thus we need to manually pre-create it.
        // Additionally rename only works across database with the same primary shard. Thus create
        // second database on the same primary shard.
        assert.commandWorked(db.adminCommand({enableSharding: dbName}));
        assert.commandWorked(db.adminCommand({
            enableSharding: otherDbName,
            primaryShard: db.getSiblingDB(dbName).getDatabasePrimaryShardId()
        }));
    }
}

function runTests(targetDbName) {
    {
        jsTest.log("Renaming a timeseries collection using the main namespace is not supported");

        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        assert.commandFailedWithCode(
            db.adminCommand(
                {renameCollection: `${dbName}.${collName}`, to: `${targetDbName}.newColl`}),
            // TODO SERVER-89100 unify error code between sharded clusters and replicaset
            [ErrorCodes.IllegalOperation, ErrorCodes.CommandNotSupportedOnView]);
    }
    {
        jsTest.log(
            "Renaming a simple collection to a bucket collection without timeseries options works");
        setupEnv();
        assert.commandWorked(db.createCollection(collName));
        assert.commandWorked(db.adminCommand({
            renameCollection: `${dbName}.${collName}`,
            to: `${targetDbName}.system.buckets.newColl`
        }));
    }
    {
        jsTest.log(
            "Renaming a timeseries collection using the main namespace with a target bucket collection fail");
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        assert.commandFailedWithCode(
            db.adminCommand({
                renameCollection: `${dbName}.${collName}`,
                to: `${dbName}.system.buckets.newColl`
            }),
            // TODO SERVER-89100 unify error code between sharded clusters and replicaset
            [ErrorCodes.IllegalOperation, ErrorCodes.CommandNotSupportedOnView]);
    }
    {
        jsTest.log("Renaming a timeseries bucket collection to a normal collection fail");
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        assert.commandFailedWithCode(
            db.adminCommand(
                {renameCollection: `${dbName}.${bucketsCollName}`, to: `${targetDbName}.newColl`}),
            ErrorCodes.IllegalOperation);
    }

    if (MongoRunner.compareBinVersions(currentFCV(), '8.1') < 0) {
        // TODO BACKPORT-19809: execute these cases in all FCV versions once the behaviour is
        // backported to 8.0
        jsTest.log(`Skipping test because FCV ${currentFCV()} is lower than 8.1`);
    } else {
        {
            jsTest.log(
                "Renaming a bucket collection without timeseries options to a normal collection works");
            setupEnv();
            assert.commandWorked(db.createCollection(bucketsCollName));
            assert.commandWorked(db.adminCommand(
                {renameCollection: `${dbName}.${bucketsCollName}`, to: `${targetDbName}.newColl`}));
        }
        {
            jsTest.log(
                "Renaming a bucket collection without timeseries options to a bucket collection works");
            setupEnv();
            assert.commandWorked(db.createCollection(bucketsCollName));
            assert.commandWorked(db.adminCommand({
                renameCollection: `${dbName}.${bucketsCollName}`,
                to: `${targetDbName}.system.buckets.newColl`
            }));
        }
    }
}

jsTest.log("Run test cases with rename within same database")
runTests(dbName);
jsTest.log("Run test cases with rename across different databases")
runTests(otherDbName);
