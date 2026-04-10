/*
 * Test the sharded fields returned by the $listClusterCatalog stage.
 *
 * @tags: [
 *    # There is no need to support multitenancy, as it has been canceled and was never in
 *    # production (see SERVER-97215 for more information)
 *    command_not_supported_in_serverless,
 *    # Avoid implicitly sharding a collection.
 *    assumes_no_implicit_collection_creation_on_get_collection,
 *    requires_getmore,
 * ]
 */

import {
    isViewlessTimeseriesOnlySuite,
    runningWithViewlessTimeseriesUpgradeDowngrade,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getRawOperationSpec} from "jstests/libs/raw_operation_utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const SHARDED_CLUSTER = 0;
const REPLICA_SET = 1;

const serverType = FixtureHelpers.isMongos(db) ? SHARDED_CLUSTER : REPLICA_SET;
const isTrackUponCreationEnabled = TestData.implicitlyTrackUnshardedCollectionOnCreation ?? false;
const collectionPlacementIsUnstable =
    TestData.runningWithBalancer === true || TestData.createsUnsplittableCollectionsOnRandomShards === true;

const dbTest = db.getSiblingDB(jsTestName());
const dbName = dbTest.getName();

const kCollUnsharded = "collUnsharded";
const kCollTimeseries = "collTimeseries";
const kView = "view";
const kCollSharded = "collSharded";
const kCollTimeseriesSharded = "collTimeseriesSharded";
const kCollTimeseriesShardedByMeta = "collTimeseriesShardedByMeta";
const kCollTimeseriesShardedByMetaSubfield = "collTimeseriesShardedByMetaSubfield";
const kTimeseriesCollections = new Set([
    kCollTimeseries,
    kCollTimeseriesSharded,
    kCollTimeseriesShardedByMeta,
    kCollTimeseriesShardedByMetaSubfield,
    getTimeseriesBucketsColl(kCollTimeseries),
    getTimeseriesBucketsColl(kCollTimeseriesSharded),
    getTimeseriesBucketsColl(kCollTimeseriesShardedByMeta),
    getTimeseriesBucketsColl(kCollTimeseriesShardedByMetaSubfield),
]);

assert.commandWorked(dbTest.dropDatabase());

// 1. Create different types of collections and fill the expectedResults dictionary at the same
// time:
//

let expectedResults = {};
expectedResults[SHARDED_CLUSTER] = {};
expectedResults[REPLICA_SET] = {};

// Standard unsharded
dbTest.createCollection(kCollUnsharded);
const primaryShard = FixtureHelpers.isMongos(dbTest) ? dbTest.getDatabasePrimaryShardId() : "";
expectedResults[SHARDED_CLUSTER][kCollUnsharded] = {
    "shards": [primaryShard],
    "tracked": isTrackUponCreationEnabled,
    "balancingEnabled": undefined,
};
expectedResults[REPLICA_SET][kCollUnsharded] = {
    "shards": [],
    "tracked": false,
    "balancingEnabled": undefined,
};

// Standard unsharded timeseries
dbTest.createCollection(kCollTimeseries, {timeseries: {timeField: "time"}});
expectedResults[SHARDED_CLUSTER][kCollTimeseries] = {
    sharded: false,
    shardKey: undefined,
    shards: [primaryShard],
    tracked: isTrackUponCreationEnabled,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined,
};
expectedResults[REPLICA_SET][kCollTimeseries] = {
    sharded: false,
    shardKey: undefined,
    shards: [],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined,
};

// View
dbTest.createView(kView, kCollUnsharded, []);
expectedResults[SHARDED_CLUSTER][kView] = {
    sharded: false,
    shardKey: undefined,
    shards: [primaryShard],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined,
};
expectedResults[REPLICA_SET][kView] = {
    sharded: false,
    shardKey: undefined,
    shards: [],
    tracked: false,
    balancingEnabled: undefined,
    balancingEnabledReason: undefined,
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
        balancingEnabledReason: {enableBalancing: true, allowMigrations: true},
    };

    // Sharded timeseries collection
    dbTest.adminCommand({
        shardCollection: dbName + "." + kCollTimeseriesSharded,
        timeseries: {timeField: "time"},
        key: {time: 1},
    });
    expectedResults[SHARDED_CLUSTER][kCollTimeseriesSharded] = {
        sharded: true,
        shardKey: {time: 1},
        shards: [primaryShard],
        tracked: true,
        balancingEnabled: true,
        balancingEnabledReason: {enableBalancing: true, allowMigrations: true},
    };

    // Sharded timeseries collection with metaField-only shard key: verifies that the "meta" ->
    // "<metaField>" conversion works when the shard key is just the metaField.
    dbTest.adminCommand({
        shardCollection: dbName + "." + kCollTimeseriesShardedByMeta,
        timeseries: {timeField: "time", metaField: "myMeta"},
        key: {myMeta: 1},
    });
    expectedResults[SHARDED_CLUSTER][kCollTimeseriesShardedByMeta] = {
        sharded: true,
        shardKey: {myMeta: 1},
        shards: [primaryShard],
        tracked: true,
        balancingEnabled: true,
        balancingEnabledReason: {enableBalancing: true, allowMigrations: true},
    };

    // Sharded timeseries collection with metaField sub-path shard key: verifies that the
    // "meta.<path>" -> "<metaField>.<path>" conversion works.
    dbTest.adminCommand({
        shardCollection: dbName + "." + kCollTimeseriesShardedByMetaSubfield,
        timeseries: {timeField: "time", metaField: "myMeta"},
        key: {"myMeta.sensorId": 1},
    });
    expectedResults[SHARDED_CLUSTER][kCollTimeseriesShardedByMetaSubfield] = {
        sharded: true,
        shardKey: {"myMeta.sensorId": 1},
        shards: [primaryShard],
        tracked: true,
        balancingEnabled: true,
        balancingEnabledReason: {enableBalancing: true, allowMigrations: true},
    };
}

// 2. Check the $listClusterCatalog output matches the expectedResults
//
const results = dbTest
    .aggregate([{$listClusterCatalog: {shards: true, tracked: true, balancingConfiguration: true}}])
    .toArray();
jsTestLog("$listClusterCatalog output: " + tojson(results));

function checkCollectionEntry(collName, expectedResult) {
    const result = results.find((collEntry) => {
        return collEntry.ns === dbName + "." + collName;
    });
    assert(result, "The collection '" + collName + "' has not been found on the $listClusterCatalog output.");

    for (const [field, value] of Object.entries(expectedResult)) {
        if (collectionPlacementIsUnstable && (field === "shards" || field === "tracked")) {
            continue;
        }
        assert.eq(
            value,
            result[field],
            "The value of the field '" + field + "' doesn't match with the expected one for the collection " + collName,
        );
    }
}

// Raw (buckets-format) shard keys for timeseries collections. system.buckets namespaces are
// treated as "raw" and always show the untranslated shard key.
const rawShardKeys = {
    [kCollTimeseriesSharded]: {"control.min.time": 1},
    [kCollTimeseriesShardedByMeta]: {meta: 1},
    [kCollTimeseriesShardedByMetaSubfield]: {"meta.sensorId": 1},
};

for (const [collName, expectedResult] of Object.entries(expectedResults[serverType])) {
    // TODO SERVER-120014: Remove this test once 9.0 becomes last LTS and all timeseries collections are viewless.
    // TODO SERVER-97061: Remove once $listClusterCatalog returns a consistent state for the local and global catalog.
    if (collName == kCollTimeseriesSharded && runningWithViewlessTimeseriesUpgradeDowngrade(dbTest)) {
        // Skip since the values of global catalog based fields (e.g. 'shards') can be inconsistent due to SERVER-97061.
        continue;
    }

    // TODO SERVER-120014: Remove this test once 9.0 becomes last LTS and all timeseries collections are viewless.
    if (results.find((collEntry) => collEntry.ns == dbName + "." + getTimeseriesBucketsColl(collName))) {
        assert(
            collName == kCollTimeseries ||
                collName == kCollTimeseriesSharded ||
                collName == kCollTimeseriesShardedByMeta ||
                collName == kCollTimeseriesShardedByMetaSubfield,
            tojson(results),
        );
        assert(!isViewlessTimeseriesOnlySuite(dbTest), tojson(results));

        // Check timeseries view
        checkCollectionEntry(collName, {
            sharded: false,
            shardKey: undefined,
            shards: serverType == SHARDED_CLUSTER ? [primaryShard] : [],
            tracked: false,
            balancingEnabled: undefined,
            balancingEnabledReason: undefined,
        });

        // For system.buckets namespaces the shard key is returned in raw (buckets) format,
        // consistent with listCollections and listIndexes.
        const bucketsExpected = Object.assign({}, expectedResult);
        if (collName in rawShardKeys) {
            bucketsExpected.shardKey = rawShardKeys[collName];
        }
        checkCollectionEntry(getTimeseriesBucketsColl(collName), bucketsExpected);
        continue;
    }

    checkCollectionEntry(collName, expectedResult);
}

// 3. Verify that rawData=true returns the raw (buckets) shard key for timeseries collections,
//    while rawData=false (default) returns the logical (user-facing) format.
if (FixtureHelpers.isMongos(dbTest)) {
    const rawResult = assert.commandWorked(
        dbTest.runCommand({
            aggregate: 1,
            pipeline: [{$listClusterCatalog: {}}, {$match: {sharded: true}}],
            cursor: {},
            ...getRawOperationSpec(dbTest),
        }),
    );
    const rawEntries = rawResult.cursor.firstBatch;

    const tsRawEntry = rawEntries.find((e) => e.ns.endsWith("." + kCollTimeseriesSharded));
    assert(tsRawEntry, "Sharded timeseries collection not found in rawData=true results: " + tojson(rawEntries));
    assert.docEq(
        {"control.min.time": 1},
        tsRawEntry.shardKey,
        "rawData=true should return the raw buckets shard key for timeseries collections",
    );

    const regularRawEntry = rawEntries.find((e) => e.ns === dbName + "." + kCollSharded);
    assert(regularRawEntry, "Sharded collection not found in rawData=true results: " + tojson(rawEntries));
    assert.docEq({x: 1}, regularRawEntry.shardKey, "rawData should not affect non-timeseries shard keys");

    const tsMetaRawEntry = rawEntries.find((e) => e.ns.endsWith("." + kCollTimeseriesShardedByMeta));
    assert(tsMetaRawEntry, kCollTimeseriesShardedByMeta + " not found in rawData=true results: " + tojson(rawEntries));
    assert.docEq(
        {meta: 1},
        tsMetaRawEntry.shardKey,
        "rawData=true should return {meta: 1} for metaField-only timeseries shard key",
    );

    const tsSubfieldRawEntry = rawEntries.find((e) => e.ns.endsWith("." + kCollTimeseriesShardedByMetaSubfield));
    assert(
        tsSubfieldRawEntry,
        kCollTimeseriesShardedByMetaSubfield + " not found in rawData=true results: " + tojson(rawEntries),
    );
    assert.docEq(
        {"meta.sensorId": 1},
        tsSubfieldRawEntry.shardKey,
        "rawData=true should return {meta.sensorId: 1} for metaField sub-path timeseries shard key",
    );
}

// 4. Test balancingEnabled and balancingEnabledReason flags with all the possible configurations.
//    This is only necessary when the cluster is sharded.
//
if (FixtureHelpers.isMongos(dbTest)) {
    function setBalancingConfiguration(collName, noBalance, permitMigrations) {
        let updateObj = {};
        updateObj["$unset"] = {};
        updateObj["$set"] = {};

        if (noBalance === undefined) {
            updateObj["$unset"]["noBalance"] = "";
        } else {
            updateObj["$set"]["noBalance"] = noBalance;
        }

        if (permitMigrations === undefined) {
            updateObj["$unset"]["permitMigrations"] = "";
        } else {
            updateObj["$set"]["permitMigrations"] = permitMigrations;
        }

        dbTest.getSiblingDB("config").collections.updateOne({_id: dbName + "." + collName}, updateObj);
    }

    function testBalancingConfiguration(collName, noBalance, permitMigrations, expectedBalancingEnabled) {
        setBalancingConfiguration(collName, noBalance, permitMigrations);

        const result = dbTest
            .aggregate([{$listClusterCatalog: {balancingConfiguration: true}}, {$match: {ns: dbName + "." + collName}}])
            .toArray();
        assert(
            result.length === 1 && result[0].ns === dbName + "." + collName,
            "Collection " + collName + " hasn't been returned by $listClusterCatalog.",
        );

        assert.eq(
            expectedBalancingEnabled,
            result[0].balancingEnabled,
            "The value of the field 'balancingEnabled' doesn't match with the expected one when `noBalance`=" +
                noBalance +
                ", and `permitMigrations`=" +
                permitMigrations,
        );

        const expectedEnableBalancing = !(noBalance ?? false);
        assert.eq(
            expectedEnableBalancing,
            result[0].balancingEnabledReason.enableBalancing,
            "The value of the field 'balancingEnabledReason.enableBalancing' (" +
                result[0].balancingEnabledReason.enableBalancing +
                ") doesn't match with the expected one when `noBalance`=" +
                noBalance +
                ", and `permitMigrations`=" +
                permitMigrations,
        );

        const expectedAllowMigrations = permitMigrations ?? true;
        assert.eq(
            expectedAllowMigrations,
            result[0].balancingEnabledReason.allowMigrations,
            "The value of the field 'balancingEnabledReason.allowMigrations' (" +
                result[0].balancingEnabledReason.allowMigrations +
                ") doesn't match with the expected one when `noBalance`=" +
                noBalance +
                ", and `permitMigrations`=" +
                permitMigrations,
        );
    }

    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ undefined,
        /* permitMigrations = */ false,
        /* expectedBalancingEnabled = */ false,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ true,
        /* permitMigrations = */ undefined,
        /* expectedBalancingEnabled = */ false,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ true,
        /* permitMigrations = */ true,
        /* expectedBalancingEnabled = */ false,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ true,
        /* permitMigrations = */ false,
        /* expectedBalancingEnabled = */ false,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ false,
        /* permitMigrations = */ undefined,
        /* expectedBalancingEnabled = */ true,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ false,
        /* permitMigrations = */ true,
        /* expectedBalancingEnabled = */ true,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ false,
        /* permitMigrations = */ false,
        /* expectedBalancingEnabled = */ false,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ undefined,
        /* permitMigrations = */ undefined,
        /* expectedBalancingEnabled = */ true,
    );
    testBalancingConfiguration(
        kCollSharded,
        /* noBalance = */ undefined,
        /* permitMigrations = */ true,
        /* expectedBalancingEnabled = */ true,
    );
}
