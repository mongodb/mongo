/*
 * Test the $_internalListCollections stage by comparing its output with the listCollections command
 * response.
 *
 * @tags: [
 *    # $_internalListCollections only supports local read concern
 *    # TODO (SERVER-98658) Reconsider this tag after resolving this ticket.
 *    assumes_read_concern_unchanged,
 *    # There is no need to support multitenancy, as it has been canceled and was never in
 *    # production (see SERVER-97215 for more information)
 *    command_not_supported_in_serverless,
 *    # listCollections is tested, so the test cannot run with garbage in config databases
 *    injected_catalog_metadata_incompatible
 * ]
 */

import {
    isViewfulTimeseriesOnlySuite,
    isViewlessTimeseriesOnlySuite,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const dbTest1 = db.getSiblingDB(jsTestName() + "1");
const dbTest2 = db.getSiblingDB(jsTestName() + "2");
const adminDB = db.getSiblingDB("admin");
const configDB = db.getSiblingDB("config");

const isBalancerEnabled = TestData.runningWithBalancer;

function removeUuidField(listOfCollections) {
    let listResult = listOfCollections.map((entry) => {
        const {
            info: {uuid: uuid, ...infoWithoutUuid},
            ...entryWithoutInfo
        } = entry;
        return {info: infoWithoutUuid, ...entryWithoutInfo};
    });
    return listResult;
}

function removePrimaryField(listOfCollections) {
    let listResult = listOfCollections.map((entry) => {
        const {primary: primary, ...entryWithoutPrimary} = entry;
        return {...entryWithoutPrimary};
    });
    return listResult;
}

function removeRecordIdsReplicatedField(listOfCollections) {
    return listOfCollections.map((entry) => {
        const {
            info: {recordIdsReplicated: _, ...infoWithoutRecordIdsReplicated},
            ...entryWithoutInfo
        } = entry;
        return {info: infoWithoutRecordIdsReplicated, ...entryWithoutInfo};
    });
}

// TODO SERVER-120014: Remove once 9.0 becomes last LTS and all timeseries collections are viewless.
function getBucketCollections(listOfCollections) {
    return listOfCollections.filter((collEntry) => collEntry["ns"].includes(".system.buckets."));
}

// TODO SERVER-120014: Remove once 9.0 becomes last LTS and all timeseries collections are viewless.
function normalizeTimeseriesCollectionFormat(listOfCollections) {
    if (isViewlessTimeseriesOnlySuite(db)) {
        return listOfCollections;
    }

    // Remove buckets collections since as those can change during viewless timeseries upgrade/downgrade
    let list = listOfCollections.filter((collEntry) => !collEntry["ns"].includes(".system.buckets."));

    // Remove the UUID field, since it is only present for viewless timeseries collections
    // (normalize so that we can compare timeseries collections regardless of the format).
    list = list.map((entry) => {
        if (entry.type !== "timeseries") {
            return entry;
        }

        const {
            info: {uuid: uuid, ...infoWithoutUuid},
            ...entryWithoutInfo
        } = entry;
        return {info: infoWithoutUuid, ...entryWithoutInfo};
    });

    return list;
}

function compareInternalListCollectionsStageAgainstListCollections(dbTest, expectedNumOfCollections) {
    // Fetch all the collections for the `dbTest` using the `listCollections` command and transform
    // them to the same format used by `$_internalListCollections`.
    let listCollectionsResponse = dbTest.getCollectionInfos().map((entry) => {
        const {name: name, ...entryWithoutName} = entry;
        return {ns: dbTest.getName() + "." + name, db: dbTest.getName(), ...entryWithoutName};
    });
    if (isBalancerEnabled) {
        // The uuid may change if there are moveCollection operations on the background, therefore
        // we don't check it.
        listCollectionsResponse = removeUuidField(listCollectionsResponse);

        // Temporary collections related to resharding may exist in the '<db>.system.*' namespace.
        // These are created by background moveCollection operations and should be filtered out
        // to pass the following checks.
        listCollectionsResponse = listCollectionsResponse.filter((collEntry) => {
            return !collEntry["ns"].includes("resharding");
        });
    }

    const listCollectionsBuckets = getBucketCollections(listCollectionsResponse);
    listCollectionsResponse = normalizeTimeseriesCollectionFormat(listCollectionsResponse);
    assert.eq(expectedNumOfCollections, listCollectionsResponse.length, listCollectionsResponse);

    // Check that all the collections returned by `listCollections` are also returned by
    // `$_internalListCollections`.
    let internalStageResponseAgainstDbTest = dbTest
        .aggregate([{$_internalListCollections: {}}, {$match: {ns: {$not: /resharding/}}}])
        .toArray();
    if (isBalancerEnabled) {
        // The uuid may change if there are moveCollection operations on the background, therefore
        // we don't check it.
        internalStageResponseAgainstDbTest = removeUuidField(internalStageResponseAgainstDbTest);
    }
    internalStageResponseAgainstDbTest = removePrimaryField(internalStageResponseAgainstDbTest);
    if (FixtureHelpers.isMongos(dbTest)) {
        // The router scrubs 'recordIdsReplicated' from listCollections responses because it is an
        // internal field that can be inconsistent across shards. $_internalListCollections returns
        // it unconditionally, so remove it before comparing.
        internalStageResponseAgainstDbTest = removeRecordIdsReplicatedField(internalStageResponseAgainstDbTest);
    }

    const internalListCollectionsBuckets = getBucketCollections(internalStageResponseAgainstDbTest);
    internalStageResponseAgainstDbTest = normalizeTimeseriesCollectionFormat(internalStageResponseAgainstDbTest);
    assert.eq(expectedNumOfCollections, internalStageResponseAgainstDbTest.length, internalStageResponseAgainstDbTest);

    assert.sameMembers(
        listCollectionsResponse,
        internalStageResponseAgainstDbTest,
        "listCollectionsResponse: " +
            tojson(listCollectionsResponse) +
            ", $_internalListCollectionsResponse: " +
            tojson(internalStageResponseAgainstDbTest),
    );

    // Check that the collections returned by listCollections are also returned by
    // $_internalListCollections when it runs against the 'admin' db.
    let stageResponseAgainstAdminDb = dbTest
        .getSiblingDB("admin")
        .aggregate([{$_internalListCollections: {}}])
        .toArray();
    if (isBalancerEnabled) {
        stageResponseAgainstAdminDb = removeUuidField(stageResponseAgainstAdminDb);
    }
    stageResponseAgainstAdminDb = removePrimaryField(stageResponseAgainstAdminDb);
    if (FixtureHelpers.isMongos(dbTest)) {
        // The router scrubs 'recordIdsReplicated' from listCollections responses because it is an
        // internal field that can be inconsistent across shards. $_internalListCollections returns
        // it unconditionally, so remove it before comparing.
        stageResponseAgainstAdminDb = removeRecordIdsReplicatedField(stageResponseAgainstAdminDb);
    }
    stageResponseAgainstAdminDb = normalizeTimeseriesCollectionFormat(stageResponseAgainstAdminDb);

    listCollectionsResponse.forEach((entry) => {
        // Use bsonUnorderedFieldsCompare rather than assert.contains so that two semantically
        // equivalent documents with different field orderings are treated as equal.
        const found = stageResponseAgainstAdminDb.some(
            (stageEntry) => bsonUnorderedFieldsCompare(entry, stageEntry) === 0,
        );
        assert(
            found,
            "The listCollections entry " +
                tojson(entry) +
                " hasn't been found on the $_internalListCollections output " +
                tojson(stageResponseAgainstAdminDb),
        );
    });

    // TODO SERVER-120014: Remove once 9.0 becomes last LTS and all timeseries collections are viewless.
    if (isViewlessTimeseriesOnlySuite(dbTest)) {
        assert.eq(0, listCollectionsBuckets.length, tojson(listCollectionsBuckets));
        assert.eq(0, internalListCollectionsBuckets.length, tojson(internalListCollectionsBuckets));
    } else if (isViewfulTimeseriesOnlySuite(dbTest)) {
        // We expect as many system.buckets collections as timeseries views
        const numTimeseries = listCollectionsResponse.filter((collEntry) => collEntry.type == "timeseries").length;
        assert.eq(numTimeseries, listCollectionsBuckets.length, tojson(listCollectionsBuckets));

        assert.sameMembers(
            listCollectionsBuckets,
            internalListCollectionsBuckets,
            "listCollectionsBuckets: " +
                tojson(listCollectionsBuckets) +
                ", $internalListCollectionsBuckets: " +
                tojson(internalListCollectionsBuckets),
        );
    }
}

function runTestOnDb(dbTest) {
    jsTestLog("Going to run the test on " + dbTest.getName());

    let numCollections = 0;

    // Non-existing db
    compareInternalListCollectionsStageAgainstListCollections(dbTest, numCollections);
    compareInternalListCollectionsStageAgainstListCollections(db.getSiblingDB("non-exising-db"), numCollections);

    // Unsharded standard collection
    assert.commandWorked(dbTest.createCollection("coll1"));
    compareInternalListCollectionsStageAgainstListCollections(dbTest, ++numCollections);

    assert.commandWorked(dbTest.createCollection("coll2"));
    compareInternalListCollectionsStageAgainstListCollections(dbTest, ++numCollections);

    // Views
    assert.commandWorked(dbTest.createView("view1", "coll1", []));
    ++numCollections; // because of `<db>.system.views`
    compareInternalListCollectionsStageAgainstListCollections(dbTest, ++numCollections);

    assert.commandWorked(dbTest.createView("view2", "coll2", []));
    compareInternalListCollectionsStageAgainstListCollections(dbTest, ++numCollections);

    // Sharded collections
    if (FixtureHelpers.isMongos(dbTest)) {
        assert.commandWorked(dbTest.adminCommand({shardCollection: dbTest.getName() + ".collSharded1", key: {x: 1}}));
        compareInternalListCollectionsStageAgainstListCollections(dbTest, ++numCollections);

        assert.commandWorked(dbTest.adminCommand({shardCollection: dbTest.getName() + ".collSharded2", key: {x: 1}}));
        compareInternalListCollectionsStageAgainstListCollections(dbTest, ++numCollections);

        // Timeseries-sharded collection
        assert.commandWorked(
            dbTest.adminCommand({
                shardCollection: dbTest.getName() + ".collShardedTim1",
                key: {t: 1},
                timeseries: {timeField: "t"},
            }),
        );
        ++numCollections;
        compareInternalListCollectionsStageAgainstListCollections(dbTest, numCollections);
    }

    // Timeseries collections
    assert.commandWorked(dbTest.createCollection("collTim1", {timeseries: {timeField: "t"}}));
    ++numCollections;
    compareInternalListCollectionsStageAgainstListCollections(dbTest, numCollections);

    assert.commandWorked(dbTest.createCollection("collTim2", {timeseries: {timeField: "t"}}));
    ++numCollections;
    compareInternalListCollectionsStageAgainstListCollections(dbTest, numCollections);
}

function runInternalCollectionsTest(dbTest) {
    // Check that $_internalListCollections returns collections for "admin" db.
    assert.soon(() => {
        const adminCollsListCollections = adminDB.getCollectionInfos();
        const adminCollsInternalListCollections = adminDB
            .aggregate([{$_internalListCollections: {}}, {$match: {db: "admin"}}])
            .toArray();

        if (adminCollsListCollections.length != adminCollsInternalListCollections.length) {
            jsTestLog(
                "The collections of the 'admin' db returned by listCollections don't match " +
                    "with the collections returned by $_internalListCollections. listCollections " +
                    "response: " +
                    tojson(adminCollsListCollections) +
                    ", $_internalListCollections response: " +
                    tojson(adminCollsInternalListCollections) +
                    ". Going to retry the comparison check.",
            );
            return false;
        }
        return true;
    }, "The collections of the 'admin' db returned by listCollections don't match with the " + "collections returned by $_internalListCollections.");

    // Check that $_internalListCollections returns "config" collections if called against "admin"
    // db.
    assert.soon(() => {
        const configCollsListCollections = configDB.getCollectionInfos();
        const configCollsInternalListCollections = adminDB
            .aggregate([{$_internalListCollections: {}}, {$match: {db: "config"}}])
            .toArray();

        if (configCollsListCollections.length != configCollsInternalListCollections.length) {
            jsTestLog(
                "The collections of the 'config' db returned by listCollections don't match " +
                    "with the collections returned by $_internalListCollections when targeting " +
                    "the 'admin' database. listCollections response: " +
                    tojson(configCollsListCollections) +
                    ", $_internalListCollections response: " +
                    tojson(configCollsInternalListCollections) +
                    ". Going to retry the comparison check.",
            );
            return false;
        }
        return true;
    }, "The collections of the 'config' db returned by listCollections don't match with the " + "collections returned by $_internalListCollections when targeting the 'admin' database.");

    // Check that $_internalListCollections returns "config" collections if called against "config"
    // db.
    assert.soon(() => {
        const configCollsListCollections = configDB.getCollectionInfos();
        const configCollsInternalListCollections = configDB.aggregate([{$_internalListCollections: {}}]).toArray();

        if (configCollsListCollections.length != configCollsInternalListCollections.length) {
            jsTestLog(
                "The collections of the 'config' db returned by listCollections don't match " +
                    "with the collections returned by $_internalListCollections when targeting " +
                    "the 'config' database. listCollections response: " +
                    tojson(configCollsListCollections) +
                    ", $_internalListCollections response: " +
                    tojson(configCollsInternalListCollections) +
                    ". Going to retry the comparison check.",
            );
            return false;
        }
        return true;
    }, "The collections of the 'config' db returned by listCollections don't match with the " + "collections returned by $_internalListCollections when targeting the 'config' database.");
}

assert.commandWorked(dbTest1.dropDatabase());
assert.commandWorked(dbTest2.dropDatabase());

runTestOnDb(dbTest1);
runTestOnDb(dbTest2);

runInternalCollectionsTest(dbTest1);
