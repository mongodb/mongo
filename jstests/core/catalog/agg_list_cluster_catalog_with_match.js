/*
 * Test that $listClusterCatalog stage behave correctly when it's followed by a $match stage.
 * We are testing this because $listClusterCatalog has been optimized when it's followed by a $match
 * that is filtering by 'db'.
 *
 * @tags: [
 *    # $listClusterCatalog was introduced in v8.1
 *    requires_fcv_81,
 *    # $listClusterCatalog only supports local read concern
 *    assumes_read_concern_unchanged,
 *    # TODO SERVER-97215: remove `command_not_supported_in_serverless`
 *    command_not_supported_in_serverless,
 *    does_not_support_transactions,
 *    # In a clustered environment, the $listClusterCatalog will eventually target the CSRS to
 *    # access the cluster catalog. The causally consistent suites run on fixtures with CSRS without
 *    # secondaries.
 *    does_not_support_causal_consistency,
 * ]
 */

const kDb1 = jsTestName() + "1";
const kDb2 = jsTestName() + "2";

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

function arrayContainsValueOnField(listClusterArrayResponse, field, value) {
    return undefined !== listClusterArrayResponse.find((collEntry) => {
        return collEntry[field] === value;
    });
}

function isTempNss(collectionName) {
    if (collectionName.includes("system.resharding") || collectionName.includes("tmp.agg_out") ||
        collectionName.includes("system.buckets.resharding")) {
        return true;
    }
    return false;
}

function testMatchStage(matchStage, expectedResults) {
    for (const dbName in expectedResults) {
        const expRes = expectedResults[dbName];
        jsTestLog("Going to test the $match stage " + tojson(matchStage) +
                  " with the following configuration: {" + dbName + ": " + tojson(expRes));

        const responseWithMatchStage =
            db.getSiblingDB(dbName).aggregate([{$listClusterCatalog: {}}, ...matchStage]).toArray();

        // If blacklisting databases/namespaces:
        if (expRes.notExpectedNamespaces !== undefined ||
            expRes.notExpectedDatabases !== undefined) {
            // Verify that the NON expected namespaces/databases are not returned by
            // $listClusterCatalog.
            const notExpectedNamespaces = expRes.notExpectedNamespaces ?? [];
            const notExpectedDatabases = expRes.notExpectedDatabases ?? [];

            for (const notExpectedNs of notExpectedNamespaces) {
                assert(!arrayContainsValueOnField(responseWithMatchStage, 'ns', notExpectedNs),
                       "The namespace " + notExpectedNs +
                           " is not expected on the output of $listClusterCatalog followed by " +
                           tojson(matchStage) + ". Output: " + tojson(responseWithMatchStage));
            }
            for (const notExpectedDb of notExpectedDatabases) {
                assert(!arrayContainsValueOnField(responseWithMatchStage, 'db', notExpectedDb),
                       "The database " + notExpectedDb +
                           " is not expected on the output of $listClusterCatalog followed by " +
                           tojson(matchStage) + ". Output: " + tojson(responseWithMatchStage));
            }

            // Verify that collections other than the non-expected ones are returned.
            // Note that we're not considering collections other than kDb1 and kDb2.
            const responseWithoutMatchStage =
                db.getSiblingDB(dbName).aggregate([{$listClusterCatalog: {}}]).toArray();

            for (const existingColl of responseWithoutMatchStage) {
                if (notExpectedNamespaces.includes(existingColl['ns'])) {
                    continue;
                }
                if (notExpectedDatabases.includes(existingColl['db']) ||
                    (existingColl['db'] !== kDb1 && existingColl['db'] !== kDb2)) {
                    continue;
                }
                if (isTempNss(existingColl['ns'])) {
                    continue;
                }

                assert(arrayContainsValueOnField(responseWithMatchStage, 'ns', existingColl.ns),
                       "The namespace " + existingColl.ns +
                           " is expected on the output of $listClusterCatalog followed by " +
                           tojson(matchStage) +
                           ". Output with $match: " + tojson(responseWithMatchStage) +
                           " Output without $match: " + tojson(responseWithoutMatchStage));
            }
        }
        // If whitelisting namespaces:
        else {
            assert(
                expRes.expectedNamespaces !== undefined && Array.isArray(expRes.expectedNamespaces),
                "`expectedNamespaces` should be specified for db " + dbName +
                    " and must be an array.");
            assert.eq(
                expRes.expectedNamespaces.length,
                responseWithMatchStage.length,
                "Expected length not match with the $listClusterCatalog response targeting db '" +
                    dbName + "' and followed by $match " + tojson(matchStage) +
                    ". Output: " + tojson(responseWithMatchStage));
            for (const collName of expRes.expectedNamespaces) {
                assert(responseWithMatchStage.find((collEntry) => collEntry['ns'] == collName),
                       "The namespace " + collName +
                           " was not found in the output of $listCatalogCluster followed by " +
                           tojson(matchStage) + ". Output: " + tojson(responseWithMatchStage));
            }
        }
    }
}

// Setting up collections for kDb1 and kDb2:
setupUserCollections(db.getSiblingDB(kDb1));
setupUserCollections(db.getSiblingDB(kDb2));

testMatchStage([{$match: {db: {$in: [kDb1, kDb2]}}}, {$match: {ns: {$regex: /coll2/}}}], {
    [kDb1]: {expectedNamespaces: [kDb1 + ".coll2"]},
    [kDb2]: {expectedNamespaces: [kDb2 + ".coll2"]},
    'admin': {expectedNamespaces: [kDb1 + ".coll2", kDb2 + ".coll2"]}
});

testMatchStage([{$match: {db: kDb1}}], {
    [kDb1]: {notExpectedNamespaces: []},
    [kDb2]: {expectedNamespaces: []},
    'admin': {notExpectedDatabases: [kDb2]}
});

testMatchStage([{$match: {db: {$ne: kDb1}}}], {
    [kDb1]: {expectedNamespaces: []},
    [kDb2]: {notExpectedNamespaces: []},
    'admin': {notExpectedDatabases: [kDb1]}
});

testMatchStage([{$match: {db: {$ne: kDb1}, ns: kDb2 + ".coll2"}}], {
    [kDb1]: {expectedNamespaces: []},
    [kDb2]: {expectedNamespaces: [kDb2 + ".coll2"]},
    'admin': {expectedNamespaces: [kDb2 + ".coll2"]}
});

testMatchStage([{$match: {ns: kDb1 + ".coll1"}}], {
    [kDb1]: {expectedNamespaces: [kDb1 + ".coll1"]},
    [kDb2]: {expectedNamespaces: []},
    'admin': {expectedNamespaces: [kDb1 + ".coll1"]}
});

testMatchStage([{$match: {ns: {$in: [kDb1 + ".coll1", kDb2 + ".coll2", kDb2 + ".view"]}}}], {
    [kDb1]: {expectedNamespaces: [kDb1 + ".coll1"]},
    [kDb2]: {expectedNamespaces: [kDb2 + ".coll2", kDb2 + ".view"]},
    'admin': {expectedNamespaces: [kDb1 + ".coll1", kDb2 + ".coll2", kDb2 + ".view"]}
});

testMatchStage([{$match: {ns: {'$nin': [kDb1 + ".coll1", kDb2 + ".coll2"]}}}], {
    [kDb1]: {notExpectedNamespaces: [kDb1 + ".coll1"]},
    [kDb2]: {notExpectedNamespaces: [kDb2 + ".coll2"]},
    'admin': {notExpectedNamespaces: [kDb1 + ".coll1", kDb2 + ".coll2"]}
});
