/*
 * Test the $listClusterCatalog stage.
 *
 * @tags: [
 *    # $listClusterCatalog was introduced in v8.1
 *    requires_fcv_81,
 *    # $listClusterCatalog only supports local read concern
 *    assumes_read_concern_unchanged,
 *    # There is no need to support multitenancy, as it has been canceled and was never in
 *    # production (see SERVER-97215 for more information)
 *    command_not_supported_in_serverless,
 *    does_not_support_transactions,
 *    # In a clustered environment, the $listClusterCatalog will eventually target the CSRS to
 *    # access the cluster catalog. The causally consistent suites run on fixtures with CSRS without
 *    # secondaries.
 *    does_not_support_causal_consistency,
 * ]
 */

const kDb1 = "db1_agg_list_cluster_catalog";
const kDb2 = "db2_agg_list_cluster_catalog";
const kNotExistent = 'notExistent_agg_list_cluster_catalog';
const kSpecsList = ["shards", "tracked", "balancingConfiguration"];

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
                let nss = expectedResult.dbName + "." + expectedResult.name;
                let nssStageResult = getStageResultForNss(stageResult, nss);
                // Temporary namespaces might disappear between the list collection request and the
                // aggregation request. Ignore this case.
                if (isTempNss(expectedResult.name) && nssStageResult == undefined) {
                    return;
                }
                // The result must be present.
                assert.neq(nssStageResult,
            undefined,
            `The namespace ${
                nss} was found in listCollections but not in the $listClusterCatalog. Result ${
                tojson(stageResult)}`);
                // The result must match the entire list collection result + some few extra fields.
                expectedResult.db = expectedResult.dbName;
                expectedResult.ns = nss;
                verify(expectedResult, nssStageResult, specs);
            });
}

function setupUserCollections(db, dbName) {
    // drop previous incaranations of the database left by previous runs of the same core test.
    db.getSiblingDB(dbName).dropDatabase();

    const kViewCollName = "view";
    const kTimeseriesCollName = "timeseries";
    const collList = ["coll1", "coll2", kTimeseriesCollName, kViewCollName];

    // Create all the collections
    collList.forEach((collName) => {
        if (collName == kTimeseriesCollName) {
            assert.commandWorked(db.getSiblingDB(dbName).createCollection(
                collName, {timeseries: {metaField: "meta", timeField: "timestamp"}}));
        } else if (collName == kViewCollName) {
            assert.commandWorked(db.getSiblingDB(dbName).createCollection(
                collName, {viewOn: "coll1", pipeline: []}));
        } else {
            db.getSiblingDB(dbName).createCollection(collName);
        }
    });
}

function runListCollectionsOnDbs(db, dbNames) {
    let result = [];
    dbNames.forEach((dbName) => {
        let dbResult = db.getSiblingDB(dbName).runCommand({listCollections: 1}).cursor.firstBatch;
        dbResult.forEach((collectionInfo) => {
            // Attach the dbName as extra information to calculate the full ns during the
            // verification step.
            collectionInfo.dbName = dbName;
            result.push(collectionInfo);
        });
    });

    return result;
}

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
    setupUserCollections(db, kDb1);

    let listCollectionResult = runListCollectionsOnDbs(db, [kDb1]);

    let stageResult = assert
                          .commandWorked(db.getSiblingDB(kDb1).runCommand(
                              {aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
                          .cursor.firstBatch;
    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return the collection for the specified user db: Using a second db.");
{
    setupUserCollections(db, kDb2);

    let listCollectionResult = runListCollectionsOnDbs(db, [kDb2]);

    let stageResult = assert
                          .commandWorked(db.getSiblingDB(kDb2).runCommand(
                              {aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
                          .cursor.firstBatch;
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

    let dbAdmin = db.getSiblingDB("admin");
    let stageResult = dbAdmin.aggregate([{$listClusterCatalog: {}}], {cursor: {}}).toArray();
    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must work under any combination of specs.");
{
    // Get the listCollections result.
    let listCollectionResult = runListCollectionsOnDbs(db, [kDb1, kDb2]);
    let dbAdmin = db.getSiblingDB("admin");

    // Test all the combination of specs.
    let allSpecs = generateSpecCombinations(kSpecsList);
    allSpecs.forEach((specs) => {
        jsTest.log("Verify the stage reports the correct result for specs " + tojson(specs));
        let stageResult = dbAdmin.aggregate([{$listClusterCatalog: specs}], {cursor: {}}).toArray();
        verifyAgainstListCollections(listCollectionResult, stageResult, specs);
    });
}
