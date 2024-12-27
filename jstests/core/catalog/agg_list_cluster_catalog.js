/*
 * Test the $listClusterCatalog stage.
 *
 * @tags: [
 *    # $listClusterCatalog was introduced in v8.1
 *    # TODO (SERVER-98651) remove the tag as part of this ticket.
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
 *    # This test invokes listCollections separately to validate the $listClusterCatalog output.
 *    # None of them can read at a provided timestamp, therefore this test cannot run in a suite
 *    # that can change a collection's incarnation.
 *    assumes_stable_collection_uuid,
 * ]
 */

const kDb1 = "db1_agg_list_cluster_catalog";
const kDb2 = "db2_agg_list_cluster_catalog";
const kNotExistent = 'notExistent_agg_list_cluster_catalog';
const kSpecsList = ["shards", "tracked", "balancingConfiguration"];
const adminDB = db.getSiblingDB("admin");

// Test all the combination of specs. Every spec can be true or false. The total number of
// combinations will be 2^n where n is number of specs.
function generateSpecCombinations(specs) {
    const totalCombinations = Math.pow(2, specs.length);
    const combinations = [];

    for (let i = 0; i < totalCombinations; i++) {
        const combination = {};
        for (let j = 0; j < specs.length; j++) {
            combination[specs[j]] = Boolean(i & (1 << j));  // 0 for false, a value for true.
        }

        combinations.push(combination);
    }
    return combinations;
}

function verify(expectedResult, result, specs) {
    // A resharding in background might change the uuid. Enforce equality.
    if (TestData.runningWithBalancer) {
        expectedResult.info.uuid = undefined;
        result.info.uuid = undefined;
    }
    // Ensure the result fields matches the expected once.
    assert.eq(expectedResult.options, result.options, "options field mismatch:" + tojson(result));
    assert.eq(expectedResult.info, result.info, "info field mismatch:" + tojson(result));
    assert.eq(expectedResult.idIndex, result.idIndex, "idIndex field mismatch:" + tojson(result));
    assert.eq(expectedResult.type, result.type, "type field mismatch:" + tojson(result));
    assert.eq(expectedResult.db, result.db, "db field mismatch:" + tojson(result));
    assert.eq(expectedResult.ns, result.ns, "ns field mismatch:" + tojson(result));
    // Ensure this field is no longer present.
    assert.eq(undefined, result.name, "name field mismatch:" + tojson(result));
    // Ensure 'sharded' field is always present.
    assert.neq(undefined, result.sharded, "sharded field mismatch:" + tojson(result));

    // Verifying the output is never undefined if the information is requested.
    // The values of the following checks might change at runtime according to the suite. The result
    // could change in between aggregation and the check (which would require querying the config
    // database).
    if (specs.tracked) {
        assert.neq(undefined, result.tracked, "tracked field mismatch:" + tojson(result));
    } else {
        assert.eq(undefined, result.tracked, "tracked field mismatch:" + tojson(result));
    }
    if (specs.shards) {
        assert.neq(undefined, result.shards, "shards field mismatch:" + tojson(result));
    } else {
        assert.eq(undefined, result.shards, "shards field mismatch:" + tojson(result));
    }
    if (result.sharded && specs.balancingConfiguration) {
        assert.neq(undefined,
                   result.balancingEnabled,
                   "balancingEnabled field mismatch:" + tojson(result));
        assert.neq(undefined,
                   result.autoMergingEnabled,
                   "autoMergingEnabled field mismatch:" + tojson(result));
        assert.neq(undefined, result.chunkSize, "chunkSize field mismatch:" + tojson(result));
    } else {
        assert.eq(undefined,
                  result.balancingEnabled,
                  "balancingEnabled field mismatch:" + tojson(result));
        assert.eq(undefined,
                  result.autoMergingEnabled,
                  "autoMergingEnabled field mismatch:" + tojson(result));
        assert.eq(undefined, result.chunkSize, "chunkSize field mismatch:" + tojson(result));
    }
}

function getStageResultForNss(stageResult, nss) {
    return stageResult.find((obj) => {
        return obj.ns === nss;
    });
}

function isTempNss(collectionName) {
    if (collectionName.startsWith("system.resharding") ||
        collectionName.startsWith("tmp.agg_out") ||
        collectionName.startsWith("system.buckets.resharding") ||
        collectionName.startsWith("system.buckets.tmp.agg_out")) {
        return true;
    }
    return false;
}

function verifyAgainstListCollections(listCollectionResult, stageResult, specs) {
    listCollectionResult
        .forEach(
            (expectedResult) => {
                let nss = expectedResult.db + "." + expectedResult.name;
                let nssStageResult = getStageResultForNss(stageResult, nss);
                // Temporary namespaces might disappear between the list collection request and the
                // aggregation request. Ignore this case.
                if (isTempNss(expectedResult.name) && nssStageResult == undefined) {
                    return;
                }
                // The result must be present.
                assert.neq(nssStageResult, undefined, `The namespace ${
                    nss} was found in listCollections but not in the $listClusterCatalog. Result ${
                    tojson(stageResult)}`);
                // The result must match the entire list collection result + some few extra fields.
                expectedResult.ns = nss;
                verify(expectedResult, nssStageResult, specs);
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
    assert.commandWorked(dbTest.createCollection(
        "timeseries", {timeseries: {metaField: "meta", timeField: "timestamp"}}));
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
        9621301);
}

jsTest.log("The stage must take an object.");
{
    assert.commandFailedWithCode(
        db.runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: "wrong"}], cursor: {}}),
        9621302);
}

jsTest.log("The stage must return the collection for the specified user db.");
{
    let listCollectionResult = runListCollectionsOnDbs(db, [kDb1]);
    let stageResult = db.getSiblingDB(kDb1).aggregate([{$listClusterCatalog: {}}]).toArray();

    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return the collection for the specified user db: Using a second db.");
{
    let listCollectionResult = runListCollectionsOnDbs(db, [kDb2]);
    let stageResult = db.getSiblingDB(kDb2).aggregate([{$listClusterCatalog: {}}]).toArray();

    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return an empty result if the user database doesn't exist.");
{
    let result =
        assert
            .commandWorked(
                db.getSiblingDB(kNotExistent)
                    .runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
            .cursor.firstBatch;
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
