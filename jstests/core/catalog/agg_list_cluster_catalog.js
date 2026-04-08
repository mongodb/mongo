/*
 * Test the $listClusterCatalog stage.
 *
 * @tags: [
 *    # There is no need to support multitenancy, as it has been canceled and was never in
 *    # production (see SERVER-97215 for more information)
 *    command_not_supported_in_serverless,
 *    # This test invokes listCollections separately to validate the $listClusterCatalog output.
 *    # None of them can read at a provided timestamp, therefore this test cannot run in a suite
 *    # that can change a collection's incarnation.
 *    assumes_stable_collection_uuid,
 *    requires_getmore,
 * ]
 */

import {isViewlessTimeseriesOnlySuite} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const kDb1 = "db1_agg_list_cluster_catalog";
const kDb2 = "db2_agg_list_cluster_catalog";
const kNotExistent = "notExistent_agg_list_cluster_catalog";
const kSpecsList = ["shards", "tracked", "balancingConfiguration"];
const adminDB = db.getSiblingDB("admin");
const configDB = db.getSiblingDB("config");

const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);

// Test all the combination of specs. Every spec can be true or false. The total number of
// combinations will be 2^n where n is number of specs.
function generateSpecCombinations(specs) {
    const totalCombinations = Math.pow(2, specs.length);
    const combinations = [];

    for (let i = 0; i < totalCombinations; i++) {
        const combination = {};
        for (let j = 0; j < specs.length; j++) {
            combination[specs[j]] = Boolean(i & (1 << j)); // 0 for false, a value for true.
        }

        combinations.push(combination);
    }
    return combinations;
}

// Verifies that the $listClusterCatalog entry (called 'stageEntry') is valid according to the given
// listCollections entry.
function verify(listCollectionEntry, stageEntry, specs) {
    // Polish both entries before comparing them.
    {
        // A resharding in background might change the uuid. Enforce equality.
        if (TestData.runningWithBalancer) {
            delete listCollectionEntry.info.uuid;
            delete stageEntry.info.uuid;
        }

        // TODO (SERVER-95599): Stop ignoring the 'info.configDebugDump' field once 9.0 becomes last LTS.
        if (isMultiversion || TestData.isRunningFCVUpgradeDowngradeSuite) {
            delete listCollectionEntry.info.configDebugDump;
            delete stageEntry.info.configDebugDump;
        }
    }

    function errMsgWithListCollectionEntry(reason) {
        return (
            reason +
            ". $listClusterCatalog entry: " +
            tojson(stageEntry) +
            ", listCollections entry: " +
            tojson(listCollectionEntry)
        );
    }

    function errMsg(reason) {
        return reason + ". $listClusterCatalog entry: " + tojson(stageEntry);
    }

    // Ensure the stageEntry fields matches the expected ones.
    assert.eq(listCollectionEntry.options, stageEntry.options, errMsgWithListCollectionEntry("options field mismatch"));
    assert.eq(listCollectionEntry.info, stageEntry.info, errMsgWithListCollectionEntry("info field mismatch"));
    assert.eq(listCollectionEntry.idIndex, stageEntry.idIndex, errMsgWithListCollectionEntry("idIndex field mismatch"));
    assert.eq(listCollectionEntry.type, stageEntry.type, errMsgWithListCollectionEntry("type field mismatch"));
    assert.eq(listCollectionEntry.db, stageEntry.db, errMsgWithListCollectionEntry("db field mismatch"));
    assert.eq(
        listCollectionEntry.db + "." + listCollectionEntry.name,
        stageEntry.ns,
        errMsgWithListCollectionEntry("ns field mismatch"),
    );

    // Ensure the field 'name' is not present.
    assert.eq(undefined, stageEntry.name, errMsg("name field should not be present"));
    // Ensure 'sharded' field is always present.
    assert.neq(undefined, stageEntry.sharded, errMsg("sharded field should always be present"));

    // Verifying the output is never undefined if the information is requested.
    // The values of the following checks might change at runtime according to the suite. The stageEntry
    // could change in between aggregation and the check (which would require querying the config
    // database).
    if (specs.tracked) {
        assert.neq(undefined, stageEntry.tracked, errMsg("tracked field should be present"));
    } else {
        assert.eq(undefined, stageEntry.tracked, errMsg("tracked field should not be present"));
    }
    if (specs.shards) {
        assert.neq(undefined, stageEntry.shards, errMsg("shards field should be present"));
    } else {
        assert.eq(undefined, stageEntry.shards, errMsg("shards field should not be present"));
    }
    if (stageEntry.sharded && specs.balancingConfiguration) {
        assert.neq(undefined, stageEntry.balancingEnabled, errMsg("balancingEnabled field should be present"));
        assert.neq(undefined, stageEntry.autoMergingEnabled, errMsg("autoMergingEnabled field should be present"));
        assert.neq(undefined, stageEntry.chunkSize, errMsg("chunkSize field should be present"));
    } else {
        assert.eq(undefined, stageEntry.balancingEnabled, errMsg("balancingEnabled field should not be present"));
        assert.eq(undefined, stageEntry.autoMergingEnabled, errMsg("autoMergingEnabled field should not be present"));
        assert.eq(undefined, stageEntry.chunkSize, errMsg("chunkSize field should not be present"));
    }
}

function isTempCollection(collectionName) {
    if (collectionName.startsWith("system.resharding") || collectionName.startsWith("tmp.agg_out")) {
        return true;
    }
    if (!isViewlessTimeseriesOnlySuite(db)) {
        if (
            collectionName.startsWith("system.buckets.resharding") ||
            collectionName.startsWith("system.buckets.tmp.agg_out")
        ) {
            return true;
        }
    }
    return false;
}

// Verifies that for every collection in the list collection result, there is a corresponding entry
// in the stage result.
function verifyAgainstListCollections(listCollectionResult, stageResult, specs) {
    listCollectionResult.forEach((listCollectionEntry) => {
        let nss = listCollectionEntry.db + "." + listCollectionEntry.name;
        let stageEntry = stageResult.find((entry) => entry.ns === nss);
        // Temporary namespaces might disappear between the list collection request and the
        // aggregation request. Ignore this case.
        if (isTempCollection(listCollectionEntry.name) && stageEntry == undefined) {
            return;
        }
        // The $listClusterCatalog entry must be present.
        assert.neq(
            stageEntry,
            undefined,
            `The namespace ${nss} was found in listCollections but not in the $listClusterCatalog. Result ${tojson(
                stageResult,
            )}`,
        );
        // The $listClusterCatalog entry must match the entire list collection result + some few extra fields.
        verify(listCollectionEntry, stageEntry, specs);
    });
}

function setupUserCollections(dbTest) {
    // Drop previous incaranations of the database left by previous runs of the same core test.
    dbTest.dropDatabase();

    // Create 4 collections of different types:
    //    - 'coll1'      : standard collection
    //    - 'coll2'      : standard collection
    //    - 'view'       : a view on 'coll1' with an empty pipeline
    //    - 'timeseries' : a timeseries collection
    assert.commandWorked(dbTest.createCollection("coll1"));
    assert.commandWorked(dbTest.createCollection("coll2"));
    assert.commandWorked(dbTest.createCollection("view", {viewOn: "coll1", pipeline: []}));
    assert.commandWorked(dbTest.createCollection("timeseries", {timeseries: {metaField: "m", timeField: "timestamp"}}));
}

function runListCollectionsOnDbs(db, dbNames) {
    let result = [];
    dbNames.forEach((dbName) => {
        let dbResult = db.getSiblingDB(dbName).getCollectionInfos();
        dbResult.forEach((collectionInfo) => {
            // Attach the dbName as extra information to calculate the full ns during the
            // verification step.
            collectionInfo.db = dbName;
            result.push(collectionInfo);
        });
    });

    return result;
}

// Setting up collections:
setupUserCollections(db.getSiblingDB(kDb1));
setupUserCollections(db.getSiblingDB(kDb2));

jsTest.log("The stage must run collectionless.");
{
    assert.commandFailedWithCode(
        db.runCommand({aggregate: "foo", pipeline: [{$listClusterCatalog: {}}], cursor: {}}),
        9621301,
    );
}

jsTest.log("The stage must take an object.");
{
    assert.commandFailedWithCode(
        db.runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: "wrong"}], cursor: {}}),
        9621302,
    );
}

jsTest.log("The stage must return the collection for the specified user db.");
{
    let listCollectionResult = runListCollectionsOnDbs(db, [kDb1]);
    let stageResult = db
        .getSiblingDB(kDb1)
        .aggregate([{$listClusterCatalog: {}}])
        .toArray();

    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return the collection for the specified user db: Using a second db.");
{
    let listCollectionResult = runListCollectionsOnDbs(db, [kDb2]);
    let stageResult = db
        .getSiblingDB(kDb2)
        .aggregate([{$listClusterCatalog: {}}])
        .toArray();

    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return an empty result if the user database doesn't exist.");
{
    let result = assert.commandWorked(
        db.getSiblingDB(kNotExistent).runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}),
    ).cursor.firstBatch;
    assert.eq(0, result.length, result);
}

jsTest.log("The stage must return every database if run against the admin db.");
{
    let listCollectionResult = runListCollectionsOnDbs(db, [kDb1, kDb2]);
    let stageResult = adminDB.aggregate([{$listClusterCatalog: {}}]).toArray();

    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must work under any combination of specs.");
{
    // Get the listCollections result.
    let listCollectionResult = runListCollectionsOnDbs(db, [kDb1, kDb2]);

    // Test all the combination of specs.
    let allSpecs = generateSpecCombinations(kSpecsList);
    allSpecs.forEach((specs) => {
        jsTest.log("Verify the stage reports the correct result for specs " + tojson(specs));
        let stageResult = adminDB.aggregate([{$listClusterCatalog: specs}]).toArray();
        verifyAgainstListCollections(listCollectionResult, stageResult, specs);
    });
}

jsTest.log("The stage must return the collections from the 'admin' database.");
{
    let listCollectionResult = runListCollectionsOnDbs(db, ["admin"]);
    let stageResult = adminDB.aggregate([{$listClusterCatalog: {}}]).toArray();

    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return the collections from the 'config' database.");
{
    assert.soon(() => {
        let listCollectionResult = runListCollectionsOnDbs(db, ["config"]);
        let stageResultAdmin = adminDB.aggregate([{$listClusterCatalog: {}}, {$match: {db: "config"}}]).toArray();
        let stageResultConfig = configDB.aggregate([{$listClusterCatalog: {}}]).toArray();
        if (
            listCollectionResult.length != stageResultAdmin.length ||
            listCollectionResult.length != stageResultConfig.length
        ) {
            return false;
        }
        verifyAgainstListCollections(listCollectionResult, stageResultAdmin, {});
        verifyAgainstListCollections(listCollectionResult, stageResultConfig, {});
        return true;
    });
}
