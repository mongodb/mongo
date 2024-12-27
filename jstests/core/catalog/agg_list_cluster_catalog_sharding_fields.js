/*
 * Test the sharded fields returned by the $listClusterCatalog stage.
 *
 * @tags: [
 *    # TODO (SERVER-98652) Remove once $listClusterCatalog is introduced in v8.0
 *    requires_fcv_81,
 *    # $listClusterCatalog only supports local read concern.
 *    # TODO (SERVER-98658) Reconsider this tag after resolving this ticket.
 *    assumes_read_concern_unchanged,
 *    # There is no need to support multitenancy, as it has been canceled and was never in
 *    # production (see SERVER-97215 for more information)
 *    command_not_supported_in_serverless,
 *    # In a clustered environment, the $listClusterCatalog will eventually target the CSRS to
 *    # access the cluster catalog. The causally consistent suites run on fixtures with CSRS without
 *    # secondaries.
 *    does_not_support_causal_consistency,
 *    # Avoid implicitly sharding a collection.
 *    assumes_no_implicit_collection_creation_on_get_collection
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const SHARDED_CLUSTER = 0;
const REPLICA_SET = 1;

const serverType = FixtureHelpers.isMongos(db) ? SHARDED_CLUSTER : REPLICA_SET;
const isTrackUponCreationEnabled = TestData.implicitlyTrackUnshardedCollectionOnCreation ?? false;
const collectionPlacementIsUnstable =
    (TestData.runningWithBalancer === true ||
     TestData.createsUnsplittableCollectionsOnRandomShards === true);

const dbTest = db.getSiblingDB(jsTestName());
const dbName = dbTest.getName();

const kCollUnsharded = "collUnsharded";
const kCollTimeseries = "collTimeseries";
const kView = "view";
const kCollSharded = "collSharded";
const kCollTimeseriesSharded = "collTimeseriesSharded";

assert.commandWorked(dbTest.dropDatabase());

// 1. Create different types of collections and fill the expectedResults dictionary at the same
// time:
//

let expectedResults = {};
expectedResults[SHARDED_CLUSTER] = {};
expectedResults[REPLICA_SET] = {};

// Standard unsharded
dbTest.createCollection(kCollUnsharded);
const primaryShard = (FixtureHelpers.isMongos(dbTest) ? dbTest.getDatabasePrimaryShardId() : "");
expectedResults[SHARDED_CLUSTER][kCollUnsharded] = {
    'shards': [primaryShard],
    'tracked': isTrackUponCreationEnabled,
    'balancingEnabled': undefined
};
expectedResults[REPLICA_SET][kCollUnsharded] = {
    'shards': [null],
    'tracked': false,
    'balancingEnabled': undefined
};

// Standard unsharded timeseries
dbTest.createCollection(kCollTimeseries, {timeseries: {timeField: 'time'}});
expectedResults[SHARDED_CLUSTER][kCollTimeseries] = {
    sharded: false,
    shardKey: undefined,
    shards: [primaryShard],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined
};
expectedResults[SHARDED_CLUSTER]["system.buckets." + kCollTimeseries] = {
    sharded: false,
    shardKey: undefined,
    shards: [primaryShard],
    tracked: isTrackUponCreationEnabled,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined
};
expectedResults[REPLICA_SET][kCollTimeseries] = {
    sharded: false,
    shardKey: undefined,
    shards: [null],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined
};
expectedResults[REPLICA_SET]["system.buckets." + kCollTimeseries] = {
    sharded: false,
    shardKey: undefined,
    shards: [null],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined
};

// View
dbTest.createView(kView, kCollUnsharded, []);
expectedResults[SHARDED_CLUSTER][kView] = {
    sharded: false,
    shardKey: undefined,
    shards: [primaryShard],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined
};
expectedResults[REPLICA_SET][kView] = {
    sharded: false,
    shardKey: undefined,
    shards: [null],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined
};

if (FixtureHelpers.isMongos(dbTest)) {
    // Sharded collection
    dbTest.adminCommand({shardCollection: dbName + "." + kCollSharded, key: {x: 1}});
    expectedResults[SHARDED_CLUSTER][kCollSharded] = {
        sharded: true,
        shardKey: {x: 1},
        shards: [primaryShard],
        tracked: true,
        balancingEnabled: true,
        balancingEnabledReason: {enableBalancing: true, allowMigrations: true}
    };

    // Sharded timeseries collection
    dbTest.adminCommand({
        shardCollection: dbName + "." + kCollTimeseriesSharded,
        timeseries: {timeField: 'time'},
        key: {time: 1}
    });
    expectedResults[SHARDED_CLUSTER][kCollTimeseriesSharded] = {
        sharded: false,
        shardKey: undefined,
        shards: [primaryShard],
        tracked: false,
        balancingEnabled: undefined,
        balancingEnabledReason: undefined
    };
    expectedResults[SHARDED_CLUSTER]["system.buckets." + kCollTimeseriesSharded] = {
        sharded: true,
        shardKey: {"control.min.time": 1},
        shards: [primaryShard],
        tracked: true,
        balancingEnabled: true,
        balancingEnabledReason: {enableBalancing: true, allowMigrations: true}
    };
}

// 2. Check the $listClusterCatalog output matches the expectedResults
//
const results =
    dbTest
        .aggregate(
            [{$listClusterCatalog: {shards: true, tracked: true, balancingConfiguration: true}}])
        .toArray();
jsTestLog("$listClusterCatalog output: " + tojson(results));

for (const [collName, expectedResult] of Object.entries(expectedResults[serverType])) {
    const result = results.find((collEntry) => {
        return collEntry.ns === dbName + "." + collName;
    });
    assert(
        result,
        "The collection '" + collName + "' has not been found on the $listClusterCatalog output.");

    for (const [field, value] of Object.entries(expectedResult)) {
        if (collectionPlacementIsUnstable && (field === 'shards' || field === 'tracked')) {
            // Can't check the 'shards' field if the collection placement isn't deterministic.
            continue;
        }
        assert.eq(value,
                  result[field],
                  "The value of the field '" + field +
                      "' doesn't match with the expected one for the collection " + collName);
    }
}

// 3. Test balancingEnabled and balancingEnabledReason flags with all the possible configurations.
//    This is only necessary when the cluster is sharded.
//
if (FixtureHelpers.isMongos(dbTest)) {
    function setBalancingConfiguration(collName, noBalance, permitMigrations) {
        let updateObj = {};
        updateObj['$unset'] = {};
        updateObj['$set'] = {};

        if (noBalance === undefined) {
            updateObj['$unset']['noBalance'] = "";
        } else {
            updateObj['$set']['noBalance'] = noBalance;
        }

        if (permitMigrations === undefined) {
            updateObj['$unset']['permitMigrations'] = "";
        } else {
            updateObj['$set']['permitMigrations'] = permitMigrations;
        }

        dbTest.getSiblingDB('config').collections.updateOne({_id: dbName + "." + collName},
                                                            updateObj);
    }

    function testBalancingConfiguration(
        collName, noBalance, permitMigrations, expectedBalancingEnabled) {
        setBalancingConfiguration(collName, noBalance, permitMigrations);

        const result = dbTest
                           .aggregate([
                               {$listClusterCatalog: {balancingConfiguration: true}},
                               {$match: {ns: dbName + "." + collName}}
                           ])
                           .toArray();
        assert(result.length === 1 && result[0].ns === dbName + "." + collName,
               "Collection " + collName + " hasn't been returned by $listClusterCatalog.");

        assert.eq(
            expectedBalancingEnabled,
            result[0].balancingEnabled,
            "The value of the field 'balancingEnabled' doesn't match with the expected one when `noBalance`=" +
                noBalance + ", and `permitMigrations`=" + permitMigrations);

        const expectedEnableBalancing = !noBalance ?? true;
        assert.eq(expectedEnableBalancing,
                  result[0].balancingEnabledReason.enableBalancing,
                  "The value of the field 'balancingEnabledReason.enableBalancing' (" +
                      result[0].balancingEnabledReason.enableBalancing +
                      ") doesn't match with the expected one when `noBalance`=" + noBalance +
                      ", and `permitMigrations`=" + permitMigrations);

        const expectedAllowMigrations = permitMigrations ?? true;
        assert.eq(expectedAllowMigrations,
                  result[0].balancingEnabledReason.allowMigrations,
                  "The value of the field 'balancingEnabledReason.allowMigrations' (" +
                      result[0].balancingEnabledReason.allowMigrations +
                      ") doesn't match with the expected one when `noBalance`=" + noBalance +
                      ", and `permitMigrations`=" + permitMigrations);
    }

    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ undefined,
                               /* permitMigrations = */ false,
                               /* expectedBalancingEnabled = */ false);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ true,
                               /* permitMigrations = */ undefined,
                               /* expectedBalancingEnabled = */ false);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ true,
                               /* permitMigrations = */ true,
                               /* expectedBalancingEnabled = */ false);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ true,
                               /* permitMigrations = */ false,
                               /* expectedBalancingEnabled = */ false);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ false,
                               /* permitMigrations = */ undefined,
                               /* expectedBalancingEnabled = */ true);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ false,
                               /* permitMigrations = */ true,
                               /* expectedBalancingEnabled = */ true);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ false,
                               /* permitMigrations = */ false,
                               /* expectedBalancingEnabled = */ false);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ undefined,
                               /* permitMigrations = */ undefined,
                               /* expectedBalancingEnabled = */ true);
    testBalancingConfiguration(kCollSharded,
                               /* noBalance = */ undefined,
                               /* permitMigrations = */ true,
                               /* expectedBalancingEnabled = */ true);
}
