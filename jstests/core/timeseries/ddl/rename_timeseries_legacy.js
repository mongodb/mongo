/*
 * Tests valid/invalid rename operations over timeseries collections
 *
 * TODO SERVER-120014: remove this test once 9.0 becomes last LTS and all timeseries collections are viewless.
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

import {
    getTimeseriesBucketsColl,
    skipTestIfViewlessTimeseriesEnabled,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getTimeseriesCollForRawOps} from "jstests/core/libs/raw_operation_utils.js";

skipTestIfViewlessTimeseriesEnabled(db);

const dbName = db.getName();
const otherDbName = `${dbName}_other`;
const collName = `coll_${jsTestName()}`;
const bucketsCollName = getTimeseriesBucketsColl(collName);
const timeseriesOpts = {
    timeseries: {timeField: "time"},
};

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
function checkSuccesfulRename(fromDBName, fromCollName, toDBName, toCollName) {
    const fromDB = db.getSiblingDB(fromDBName);
    const toDB = db.getSiblingDB(toDBName);
    const fromColl = fromDB[fromCollName];
    const toColl = toDB[toCollName];

    // 4 measurements falling into 3 different buckets
    const measurements = [
        {time: ISODate("2019-01-30T07:30:10.957Z"), meta: 0},
        {time: ISODate("2019-01-30T07:30:11.957Z"), meta: 1},
        {time: ISODate("2020-01-30T07:30:11.957Z"), meta: 2},
        {time: ISODate("2021-01-30T07:30:11.957Z"), meta: 3},
    ];

    for (let i = 0; i < measurements.length; i++) {
        assert.commandWorked(fromColl.insert(measurements[i]));
    }

    const beforeBuckets = getTimeseriesCollForRawOps(fromColl).find().rawData().toArray();

    assert.commandWorked(
        db.adminCommand({
            renameCollection: getTimeseriesBucketsColl(fromColl).getFullName(),
            to: getTimeseriesBucketsColl(toColl).getFullName(),
        }),
    );

    // Fix view definitions following rename
    fromColl.drop();
    assert.commandWorked(toDB.createCollection(toColl.getName(), timeseriesOpts));

    const afterBuckets = getTimeseriesCollForRawOps(toColl).find().rawData().toArray();
    assert.eq(beforeBuckets, afterBuckets);

    const afterMeasurements = toColl.find({}, {_id: 0}).sort({time: 1}).toArray();
    assert.eq(afterMeasurements, measurements);
}

function runSystemBucketsTests(targetDbName) {
    {
        jsTest.log("Renaming a timeseries collection using the main namespace is not supported");

        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        assert.commandFailedWithCode(
            db.adminCommand({
                renameCollection: `${dbName}.${collName}`,
                to: `${targetDbName}.newColl`,
            }),
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
            to: `${targetDbName}.${getTimeseriesBucketsColl("newColl")}`,
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
                to: `${dbName}.${getTimeseriesBucketsColl("newColl")}`,
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
            db.adminCommand({
                renameCollection: `${dbName}.${bucketsCollName}`,
                to: `${targetDbName}.newColl`,
            }),
            ErrorCodes.IllegalOperation,
        );
    }

    {
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        const isUnsharded =
            db.getSiblingDB("config").collections.countDocuments({
                _id: `${dbName}.${bucketsCollName}`,
                unsplittable: {$ne: true},
            }) == 0;
        // Rename across db is not supported for sharded collections
        if (dbName == targetDbName || isUnsharded) {
            jsTest.log("Renaming a timeseries bucket collection to another bucket collection works");
            checkSuccesfulRename(dbName, collName, targetDbName, "newColl");
        } else {
            jsTest.log("Renaming a sharded timeseries bucket collection to a different db fails");

            assert.commandFailedWithCode(
                db.adminCommand({
                    renameCollection: `${dbName}.${bucketsCollName}`,
                    to: `${targetDbName}.${getTimeseriesBucketsColl("newColl")}`,
                }),
                ErrorCodes.CommandFailed,
            );
        }
    }
}

jsTest.log("Run test cases with rename within same database");
runSystemBucketsTests(dbName);

jsTest.log("Run test cases with rename across different databases");
runSystemBucketsTests(otherDbName);
