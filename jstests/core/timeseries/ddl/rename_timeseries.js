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
const collName = `coll_${jsTestName()}`;
const bucketsCollName = `system.buckets.${collName}`;
const timeseriesOpts = {
    timeseries: {timeField: "time"},
};

// TODO SERVER-101595 get rid of all the code protected behind this flag
const viewlessTimeseriesEnabled = FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections");

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
        assert.commandWorked(
            db.adminCommand({
                enableSharding: otherDbName,
                primaryShard: db.getSiblingDB(dbName).getDatabasePrimaryShardId(),
            }),
        );
    }
}

// Insert measurements -> rename -> check measurements/buckets have not changed
function checkSuccessfullRename(fromDb, fromColl, toDb, toColl) {
    const srcNss = fromDb + (viewlessTimeseriesEnabled ? "." : ".system.buckets.") + fromColl;
    const srcColl = db.getSiblingDB(fromDb)[fromColl];
    const dstNss = toDb + (viewlessTimeseriesEnabled ? "." : ".system.buckets.") + toColl;
    const dstColl = db.getSiblingDB(toDb)[toColl];

    // 4 measurements falling into 3 different buckets
    const measurements = [
        {time: ISODate("2019-01-30T07:30:10.957Z"), meta: 0},
        {time: ISODate("2019-01-30T07:30:11.957Z"), meta: 1},
        {time: ISODate("2020-01-30T07:30:11.957Z"), meta: 2},
        {time: ISODate("2021-01-30T07:30:11.957Z"), meta: 3},
    ];

    for (let i = 0; i < measurements.length; i++) {
        assert.commandWorked(srcColl.insert(measurements[i]));
    }

    const beforeBuckets = viewlessTimeseriesEnabled ? srcColl.find().rawData().toArray() : srcColl.find().toArray();

    assert.commandWorked(db.adminCommand({renameCollection: srcNss, to: dstNss}));

    if (!viewlessTimeseriesEnabled) {
        // Fix view definitions following rename
        assert.commandWorked(db.getSiblingDB(fromDb).runCommand({drop: fromColl}));
        assert.commandWorked(db.getSiblingDB(toDb).createCollection(toColl, timeseriesOpts));
    }

    const afterBuckets = viewlessTimeseriesEnabled ? dstColl.find().rawData().toArray() : dstColl.find().toArray();
    assert.eq(beforeBuckets, afterBuckets);

    const afterMeasurements = dstColl.find({}, {_id: 0}).sort({time: 1}).toArray();
    assert.eq(afterMeasurements, measurements);
}

function runSystemBucketsTests(targetDbName) {
    {
        jsTest.log("Renaming a timeseries collection using the main namespace is not supported");

        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        assert.commandFailedWithCode(
            db.adminCommand({renameCollection: `${dbName}.${collName}`, to: `${targetDbName}.newColl`}),
            // TODO SERVER-89100 unify error code between sharded clusters and replicaset
            [ErrorCodes.IllegalOperation, ErrorCodes.CommandNotSupportedOnView],
        );
    }
    {
        jsTest.log("Renaming a simple collection to a bucket collection without timeseries options fails");
        setupEnv();
        assert.commandWorked(db.createCollection(collName));
        const res = db.adminCommand({
            renameCollection: `${dbName}.${collName}`,
            to: `${targetDbName}.system.buckets.newColl`,
        });
        assert.commandFailedWithCode(res, [ErrorCodes.IllegalOperation]);
    }
    {
        jsTest.log("Renaming a timeseries collection using the main namespace with a target bucket collection fail");
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        assert.commandFailedWithCode(
            db.adminCommand({
                renameCollection: `${dbName}.${collName}`,
                to: `${dbName}.system.buckets.newColl`,
            }),
            // TODO SERVER-89100 unify error code between sharded clusters and replicaset
            [ErrorCodes.IllegalOperation, ErrorCodes.CommandNotSupportedOnView],
        );
    }
    {
        jsTest.log("Renaming a timeseries bucket collection to a normal collection fail");
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        assert.commandFailedWithCode(
            db.adminCommand({renameCollection: `${dbName}.${bucketsCollName}`, to: `${targetDbName}.newColl`}),
            ErrorCodes.IllegalOperation,
        );
    }

    {
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        const isUnsharded =
            db
                .getSiblingDB("config")
                .collections.countDocuments({_id: `${dbName}.${bucketsCollName}`, unsplittable: {$ne: true}}) == 0;
        // Rename across db is not supported for sharded collections
        if (dbName == targetDbName || isUnsharded) {
            jsTest.log("Renaming a timeseries bucket collection to another bucket collection works");
            checkSuccessfullRename(dbName, collName, targetDbName, "newColl");
        } else {
            jsTest.log("Renaming a sharded timeseries bucket collection to a different db fails");

            assert.commandFailedWithCode(
                db.adminCommand({
                    renameCollection: `${dbName}.${bucketsCollName}`,
                    to: `${targetDbName}.system.buckets.newColl`,
                }),
                ErrorCodes.CommandFailed,
            );
        }
    }
}

function runViewLessTimeseriesTests(targetDbName) {
    {
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        const isUnsharded =
            db
                .getSiblingDB("config")
                .collections.countDocuments({_id: `${dbName}.${collName}`, unsplittable: {$ne: true}}) == 0;

        // Rename across db is not supported for sharded collections
        if (dbName == targetDbName || isUnsharded) {
            jsTest.log("Renaming a viewless timeseries collection works");
            checkSuccessfullRename(dbName, collName, targetDbName, "newColl");
        } else {
            jsTest.log("Renaming a sharded viewless timeseries collection to a different db fails");
            assert.commandFailedWithCode(
                db.adminCommand({
                    renameCollection: `${dbName}.${bucketsCollName}`,
                    to: `${targetDbName}.newColl`,
                }),
                ErrorCodes.IllegalOperation,
            );
        }
    }

    {
        jsTest.log("Renaming a viewless timeseries collection to a target bucket collection fails");
        setupEnv();
        assert.commandWorked(db.createCollection(collName));
        const res = db.adminCommand({
            renameCollection: `${dbName}.${collName}`,
            to: `${targetDbName}.system.buckets.newColl`,
        });
        assert.commandFailedWithCode(res, [ErrorCodes.IllegalOperation]);
    }
}

jsTest.log("Run test cases with rename within same database");
if (viewlessTimeseriesEnabled) {
    runViewLessTimeseriesTests(dbName);
} else {
    runSystemBucketsTests(dbName);
}

jsTest.log("Run test cases with rename across different databases");
if (viewlessTimeseriesEnabled) {
    runViewLessTimeseriesTests(otherDbName);
} else {
    runSystemBucketsTests(otherDbName);
}
