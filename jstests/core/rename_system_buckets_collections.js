/*
 * Tests valid/invalid rename operations over timeseries collections
 *
 * @tags: [
 *  uses_rename,
 *  requires_timeseries,
 *  # the rename command is not idempotent
 *  requires_non_retryable_commands,
 *  # Assumes FCV remain stable during the entire duration of the test
 *  # TODO SERVER-89999: remove once we stop using FeatureFlagUtil.isEnabled
 *  cannot_run_during_upgrade_downgrade,
 *  # rename only works across databases with same primary shard
 *  # TODO SERVER-90096: change this tag with a more specific one
 *  assumes_balancer_off,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const dbName = db.getName();
const otherDbName = `${dbName}_other`;
const collName = `coll_${jsTestName()}`;
const bucketsCollName = `system.buckets.${collName}`;
const timeseriesOpts = {
    timeseries: {timeField: "time"}
};

// TODO SERVER-89999: remove once the feature flag version becomes last LTS
const simpleBucketCollectionsDisallowed =
    FeatureFlagUtil.isEnabled(db, "DisallowBucketCollectionWithoutTimeseriesOptions")

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
        const res = db.adminCommand({
            renameCollection: `${dbName}.${collName}`,
            to: `${targetDbName}.system.buckets.newColl`
        });
        if (simpleBucketCollectionsDisallowed) {
            assert.commandFailedWithCode(res, [ErrorCodes.IllegalOperation]);
        } else {
            assert.commandWorked(res);
        }
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

    if (simpleBucketCollectionsDisallowed) {
        jsTest.log(
            "Skipping test cases that needs creating bucket collection without timeseries options because it is not supported in current FCV version");
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
