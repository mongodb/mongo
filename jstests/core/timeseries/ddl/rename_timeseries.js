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
 *  # Renaming a timeseries collection is only possible with viewless timeseries collections,
 *  # with legacy viewful timeseries collection it was not supported.
 *  featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const dbName = db.getName();
const otherDbName = `${dbName}_other`;
const collName = `coll_${jsTestName()}`;
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

    const beforeBuckets = fromColl.find().rawData().toArray();

    assert.commandWorked(
        db.adminCommand({
            renameCollection: fromColl.getFullName(),
            to: toColl.getFullName(),
        }),
    );

    const afterBuckets = toColl.find().rawData().toArray();
    assert.eq(beforeBuckets, afterBuckets);

    const afterMeasurements = toColl.find({}, {_id: 0}).sort({time: 1}).toArray();
    assert.eq(afterMeasurements, measurements);
}

function runTimeseriesTests(targetDbName) {
    {
        setupEnv();
        assert.commandWorked(db.createCollection(collName, timeseriesOpts));
        const isUnsharded =
            db.getSiblingDB("config").collections.countDocuments({
                _id: `${dbName}.${collName}`,
                unsplittable: {$ne: true},
            }) == 0;

        // Rename across db is not supported for sharded collections
        if (dbName == targetDbName || isUnsharded) {
            jsTest.log("Renaming a viewless timeseries collection works");
            checkSuccesfulRename(dbName, collName, targetDbName, "newColl");
        } else {
            jsTest.log("Renaming a sharded viewless timeseries collection to a different db fails");
            assert.commandFailedWithCode(
                db.adminCommand({
                    renameCollection: `${dbName}.${collName}`,
                    to: `${targetDbName}.newColl`,
                }),
                ErrorCodes.CommandFailed,
            );
        }
    }

    {
        jsTest.log("Renaming a viewless timeseries collection to a target bucket collection fails");
        setupEnv();
        assert.commandWorked(db.createCollection(collName));
        const res = db.adminCommand({
            renameCollection: `${dbName}.${collName}`,
            to: `${targetDbName}.${getTimeseriesBucketsColl("newColl")}`,
        });
        assert.commandFailedWithCode(res, [ErrorCodes.IllegalOperation]);
    }
}

jsTest.log.info("Run test cases with rename within same database");
runTimeseriesTests(dbName);

jsTest.log.info("Run test cases with rename across different databases");
runTimeseriesTests(otherDbName);
